/*
 * LocalHost hypervisor -- DSDT (Differentiated System Description Table)
 *
 * WHY THIS EXISTS (U49, 2026-07-28)
 * ---------------------------------
 * Until now acpiBuildTables() emitted a 36-byte DSDT: a bare ACPI header with no
 * AML definition block at all. U48 showed the consequence -- every boot, 40/40,
 * bugchecks 0xA5 ACPI_BIOS_ERROR (p1=0x11 p2=0x3) raised from acpi.sys (confirmed
 * by resolving the bugcheck caller against acpi.pdb), and the guest never writes a
 * single disk sector. Windows' ACPI driver cannot enumerate a machine that
 * declares no namespace.
 *
 * This file is the real namespace. It is deliberately minimal: it declares only
 * what this hypervisor actually emulates, because declaring hardware we do not
 * emulate is exactly how you get a different, harder-to-diagnose ACPI failure.
 *
 * WHAT THIS MACHINE ACTUALLY IS (see Hypervisor.c)
 * -----------------------------------------------
 *   PCI bus 0, device 0 func 0  i440FX-style host bridge   (pciHostBridgeConfig)
 *   PCI bus 0, device 1 func 0  PIIX-style ISA bridge      (pciIsaBridgeConfig)
 *   PCI bus 0, device 2 func 0  AHCI controller            (pciAhciConfig)
 *   PCI bus 0, device 3 func 0  RTL8139 NIC                (pciRtl8139Config)
 *   Guest RAM      3 GB in UEFI mode (UEFI_GUEST_RAM_SIZE), so the PCI MMIO
 *                  hole starts at 0xC0000000 (where the AHCI ABAR is mapped)
 *   I/O APIC       0xFEC00000, GSI 0-23   (second one at 0xFEC01000, GSI 24-47)
 *   Local APIC     0xFEE00000
 *   SCI            IRQ 9 / GSI 9 (FADT.SCI_INT, with a MADT source override)
 *   PM registers   PIIX4-style, base read from the ISA bridge's config 0x40
 *
 * CAVEAT ON _PRT (measured, not assumed)
 * --------------------------------------
 * Device interrupts in this hypervisor are injected as HARDCODED legacy PIC
 * vectors -- 0x76 for AHCI (IRQ14), 0x73 for the NIC (IRQ11), 0x09 keyboard,
 * 0x74 mouse -- straight into the vCPU, bypassing both the emulated PIC and the
 * I/O APIC redirection tables. The only exception is GSI 8, where
 * ioapicResolveVector() honours a guest-programmed vector.
 *
 * So the _PRT below is DECLARATIVE ONLY today: ACPI needs it to build a coherent
 * namespace, but changing it will not change where interrupts actually land until
 * the injection path is taught to consult the I/O APIC redirection entries. That
 * is a separate piece of work and is called out as such rather than pretended
 * away. The GSI numbers chosen (16-19) are the conventional PCI block and sit
 * inside our first I/O APIC's 0-23 range.
 */

DefinitionBlock ("dsdt.aml", "DSDT", 2, "LCLHST", "LHVMDSDT", 0x00000001)
{
    Scope (\_SB)
    {
        /*
         * PCI root bridge. PNP0A03 is the plain PCI root; PNP0A08 (PCIe) is
         * deliberately NOT claimed as a _CID, because we present no ECAM/MCFG
         * region -- claiming PCIe would invite the OS to look for one.
         */
        Device (PCI0)
        {
            /*
             * _HID and NOT _ADR: iasl warning 3073 -- a Device takes one or the
             * other. The root bridge is enumerated by ACPI itself via _HID, not
             * by a parent bus via an address, so _ADR would be wrong here (it is
             * correct on the child devices below, which sit on PCI bus 0).
             */
            Name (_HID, EisaId ("PNP0A03"))
            Name (_UID, Zero)
            Name (_BBN, Zero)          /* base bus number 0 */

            Name (_CRS, ResourceTemplate ()
            {
                /* Bus numbers this bridge decodes. */
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000, 0x0000, 0x00FF, 0x0000, 0x0100)

                /* The PCI configuration mechanism-1 ports, consumed by us. */
                IO (Decode16, 0x0CF8, 0x0CF8, 0x01, 0x08)

                /* I/O space below and above the config ports. */
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000, 0x0000, 0x0CF7, 0x0000, 0x0CF8)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000, 0x0D00, 0xFFFF, 0x0000, 0xF300)

                /*
                 * PCI MMIO hole: from the top of the 3 GB of guest RAM up to the
                 * start of the LAPIC/IOAPIC region at 0xFEC00000. The AHCI ABAR
                 * lives at 0xC0000000, inside this window.
                 */
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x00000000, 0xC0000000, 0xFEBFFFFF, 0x00000000, 0x3EC00000)
            })

            /*
             * Interrupt routing, APIC mode. Source is Zero and the source index
             * is the GSI directly, which is the standard way to express "route
             * through the I/O APIC" without needing PCI link (LNKx) devices.
             * Only INTA# of each real device is declared, since that is all any
             * of them assert.
             */
            Name (_PRT, Package ()
            {
                Package () { 0x0002FFFF, Zero, Zero, 16 },   /* dev 2, AHCI  INTA# -> GSI 16 */
                Package () { 0x0003FFFF, Zero, Zero, 17 },   /* dev 3, NIC   INTA# -> GSI 17 */
                Package () { 0x0001FFFF, Zero, Zero, 18 },   /* dev 1, ISA   INTA# -> GSI 18 */
                Package () { 0x0000FFFF, Zero, Zero, 19 }    /* dev 0, host  INTA# -> GSI 19 */
            })

            /*
             * The PIIX-style ISA bridge, and under it the legacy devices this
             * hypervisor genuinely emulates. Windows expects to find the PS/2
             * controller, RTC and timer declared somewhere in the namespace.
             */
            Device (ISA0)
            {
                Name (_ADR, 0x00010000)     /* device 1, function 0 */

                /* PS/2 keyboard -- emulated (port 0x60/0x64, IRQ1). */
                Device (KBD0)
                {
                    Name (_HID, EisaId ("PNP0303"))
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0060, 0x0060, 0x01, 0x01)
                        IO (Decode16, 0x0064, 0x0064, 0x01, 0x01)
                        IRQNoFlags () { 1 }
                    })
                }

                /* PS/2 mouse -- emulated (aux port, IRQ12). */
                Device (MOU0)
                {
                    Name (_HID, EisaId ("PNP0F13"))
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IRQNoFlags () { 12 }
                    })
                }

                /* Real-time clock / CMOS -- emulated (0x70/0x71, IRQ8). */
                Device (RTC0)
                {
                    Name (_HID, EisaId ("PNP0B00"))
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0070, 0x0070, 0x01, 0x02)
                        IRQNoFlags () { 8 }
                    })
                }

                /* 8254 PIT -- emulated (0x40-0x43, IRQ0). */
                Device (TMR0)
                {
                    Name (_HID, EisaId ("PNP0100"))
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0040, 0x0040, 0x01, 0x04)
                        IRQNoFlags () { 0 }
                    })
                }

                /*
                 * COM2 at 0x2F8, IRQ3. This is the port the DBG2 table points
                 * the kernel debugger at, so it must exist in the namespace too
                 * -- DBG2 currently uses the "." placeholder precisely because
                 * there was no namespace device to name.
                 */
                Device (UAR2)
                {
                    Name (_HID, EisaId ("PNP0501"))
                    Name (_UID, 0x02)
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x02F8, 0x02F8, 0x01, 0x08)
                        IRQNoFlags () { 3 }
                    })
                }
            }
        }

        /*
         * The single processor. _UID must match the ACPI Processor UID in the
         * MADT's Processor Local APIC entry (which is 0). ACPI0007 is the
         * modern device-style declaration, replacing the deprecated Processor()
         * opcode.
         */
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, Zero)
            Name (_STA, 0x0F)
        }
    }

    /*
     * S5 (soft off). Windows wants this to shut down rather than reporting that
     * the machine cannot power off. The first element is the PM1a SLP_TYP value;
     * our PM1a_CNT emulation treats a write with SLP_EN as a power-off request.
     */
    Name (\_S5, Package (0x02)
    {
        0x05,       /* PM1a_CNT.SLP_TYP */
        0x00        /* PM1b_CNT.SLP_TYP (no PM1b block) */
    })
}
