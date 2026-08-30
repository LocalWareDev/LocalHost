// Intel 82540EM (e1000) NIC -- PCI 0:5.0, GSI 21. Backup adapter; the
// RTL8139 on 0:3.0 remains the primary NIC. Same slirp backend via
// netReceiveFrame / g_netTransmit.
//
// Register map and descriptor formats follow the 8254x family datasheet and
// the same subset QEMU's hw/net/e1000.c exposes: CTRL/STATUS, EEPROM (EERD),
// MDI PHY, ICR/ICS/IMS/IMC, RCTL/TCTL, RX/TX rings, RA[0]. BAR0 is 128KB
// MMIO left unmapped so every access faults (ICR is read-to-clear; TDT is
// the doorbell). BAR1 is a 64-byte I/O window (IOADDR/IODATA).

#define E1000_MMIO_SIZE     0x20000
#define E1000_IO_SIZE       0x40
#define E1000_EEPROM_WORDS  64

#define E1000_CTRL     0x00000
#define E1000_STATUS   0x00008
#define E1000_EECD     0x00010
#define E1000_EERD     0x00014
#define E1000_CTRL_EXT 0x00018
#define E1000_MDIC     0x00020
#define E1000_FCAL     0x00028
#define E1000_FCAH     0x0002C
#define E1000_FCT      0x00030
#define E1000_VET      0x00038
#define E1000_ICR      0x000C0
#define E1000_ITR      0x000C4
#define E1000_ICS      0x000C8
#define E1000_IMS      0x000D0
#define E1000_IMC      0x000D8
#define E1000_RCTL     0x00100
#define E1000_TCTL     0x00400
#define E1000_TIPG     0x00410
#define E1000_PBA      0x01000
#define E1000_RDBAL    0x02800
#define E1000_RDBAH    0x02804
#define E1000_RDLEN    0x02808
#define E1000_RDH      0x02810
#define E1000_RDT      0x02818
#define E1000_RDTR     0x02820
#define E1000_TDBAL    0x03800
#define E1000_TDBAH    0x03804
#define E1000_TDLEN    0x03808
#define E1000_TDH      0x03810
#define E1000_TDT      0x03818
#define E1000_TIDV     0x03820
#define E1000_RA       0x05400
#define E1000_MTA      0x05200
#define E1000_VFTA     0x05600

#define E1000_CTRL_FD      0x00000001
#define E1000_CTRL_SLU     0x00000040
#define E1000_CTRL_RST     0x04000000
#define E1000_CTRL_PHY_RST 0x80000000

#define E1000_STATUS_FD    0x00000001
#define E1000_STATUS_LU    0x00000002
#define E1000_STATUS_SPEED_1000 0x00000080

#define E1000_RCTL_EN      0x00000002
#define E1000_RCTL_UPE     0x00000008
#define E1000_RCTL_MPE     0x00000010
#define E1000_RCTL_BAM     0x00008000
#define E1000_RCTL_SECRC   0x04000000

#define E1000_TCTL_EN      0x00000002

#define E1000_ICR_TXDW     0x00000001
#define E1000_ICR_TXQE     0x00000002
#define E1000_ICR_LSC      0x00000004
#define E1000_ICR_RXO      0x00000040
#define E1000_ICR_RXT0     0x00000080

#define E1000_TXD_CMD_EOP  0x01
#define E1000_TXD_CMD_IFCS 0x02
#define E1000_TXD_CMD_RS   0x08
#define E1000_TXD_CMD_DEXT 0x20
#define E1000_TXD_STAT_DD  0x01

#define E1000_RXD_STAT_DD  0x01
#define E1000_RXD_STAT_EOP 0x02

#define E1000_TX_BUF_MAX   65536

extern unsigned char e1000Mac[6];

UINT32 e1000Reg[E1000_MMIO_SIZE / 4];
UINT16 e1000Eeprom[E1000_EEPROM_WORDS];
UINT16 e1000Phy[32];

int e1000Bar0Sizing = 0;
UINT32 e1000MmioBase = 0;
int e1000BarMapped = 0;

int e1000Bar1Sizing = 0;
UINT32 e1000IoBase = 0;
UINT32 e1000IoAddr = 0;

int pendingE1000Irq = 0;
long g_e1000MmioReads = 0, g_e1000MmioWrites = 0, g_e1000Tx = 0, g_e1000Rx = 0;
long g_e1000RxDrop = 0, g_e1000Irq = 0;

unsigned char e1000TxAcc[E1000_TX_BUF_MAX];
UINT32 e1000TxAccLen = 0;
UINT32 e1000TxMss = 0;
UINT32 e1000TxHdrlen = 0;
int e1000TxTse = 0;

static UINT32 e1000R(UINT32 off) {
    off &= (E1000_MMIO_SIZE - 1);
    return e1000Reg[off >> 2];
}
static void e1000W(UINT32 off, UINT32 val) {
    off &= (E1000_MMIO_SIZE - 1);
    e1000Reg[off >> 2] = val;
}

static int e1000GuestCopy(UINT64 gpa, void *buf, UINT32 len, int toGuest) {
    if (!guestMemory || len == 0) return 0;
    if (gpa + (UINT64)len > (UINT64)guestMemSize) return 0;
    if (toGuest) memcpy((unsigned char *)guestMemory + (SIZE_T)gpa, buf, len);
    else memcpy(buf, (unsigned char *)guestMemory + (SIZE_T)gpa, len);
    return 1;
}

static void e1000EepromInit(void) {
    UINT32 i, sum = 0;
    memset(e1000Eeprom, 0, sizeof(e1000Eeprom));
    e1000Eeprom[0] = (UINT16)(e1000Mac[0] | ((UINT16)e1000Mac[1] << 8));
    e1000Eeprom[1] = (UINT16)(e1000Mac[2] | ((UINT16)e1000Mac[3] << 8));
    e1000Eeprom[2] = (UINT16)(e1000Mac[4] | ((UINT16)e1000Mac[5] << 8));
    e1000Eeprom[0x0A] = 0x100E; // device ID
    e1000Eeprom[0x0B] = 0x8086; // subsystem vendor
    e1000Eeprom[0x0C] = 0x001E; // subsystem ID (82540EM copper)
    for (i = 0; i < E1000_EEPROM_WORDS - 1; i++) sum += e1000Eeprom[i];
    e1000Eeprom[E1000_EEPROM_WORDS - 1] = (UINT16)(0xBABA - sum);
}

static void e1000PhyInit(void) {
    memset(e1000Phy, 0, sizeof(e1000Phy));
    e1000Phy[0] = 0x1140;  // BMCR: auto-neg, 1000
    e1000Phy[1] = 0x796D;  // BMSR: link up, autoneg complete, capabilities
    e1000Phy[2] = 0x0141;  // PHY ID1 (Marvell-compatible, matches QEMU 82540)
    e1000Phy[3] = 0x0C24;  // PHY ID2
    e1000Phy[4] = 0x0DE1;  // ANAR
    e1000Phy[5] = 0x45E1;  // ANLPAR: partner 1000/full
    e1000Phy[9] = 0x0E00;  // 1000T control
    e1000Phy[10] = 0x3C00; // 1000T status: local/remote RX OK
}

void e1000Reset(void) {
    memset(e1000Reg, 0, sizeof(e1000Reg));
    e1000TxAccLen = 0;
    e1000TxMss = 0;
    e1000TxHdrlen = 0;
    e1000TxTse = 0;
    e1000EepromInit();
    e1000PhyInit();
    e1000W(E1000_CTRL, E1000_CTRL_FD | E1000_CTRL_SLU | 0x00000100);
    // LU + FD + 1000Mb + PCI-X/GIO-master bits Windows' e1k polls after reset.
    e1000W(E1000_STATUS, 0x80080783);
    e1000W(E1000_PBA, 0x00100030);
    e1000W(E1000_VET, 0x8100);
    e1000W(E1000_TIPG, 0x00602008);
    e1000W(E1000_EECD, 0x00000100); // EE_PRES
    e1000W(E1000_RA + 0, (UINT32)e1000Mac[0] | ((UINT32)e1000Mac[1] << 8)
           | ((UINT32)e1000Mac[2] << 16) | ((UINT32)e1000Mac[3] << 24));
    e1000W(E1000_RA + 4, (UINT32)e1000Mac[4] | ((UINT32)e1000Mac[5] << 8) | 0x80000000u);
    e1000W(E1000_ICR, E1000_ICR_LSC);
}

int e1000RxEnabled(void) {
    return (e1000R(E1000_RCTL) & E1000_RCTL_EN) ? 1 : 0;
}

static UINT32 e1000RxDescCount(void) {
    UINT32 rdlen = e1000R(E1000_RDLEN);
    return rdlen / 16;
}

int e1000RxCanAccept(UINT32 bytes) {
    UINT32 n, rdh, rdt, unused;
    (void)bytes;
    if (!e1000RxEnabled()) return 0;
    n = e1000RxDescCount();
    if (n == 0) return 0;
    rdh = e1000R(E1000_RDH);
    rdt = e1000R(E1000_RDT);
    if (rdh >= n || rdt >= n) return 0;
    unused = (rdt + n - rdh) % n;
    return unused > 1; // hardware leaves one slot empty
}

static void e1000MaybeInjectIrq(WHV_PARTITION_HANDLE partition) {
    UINT32 icr = e1000R(E1000_ICR);
    UINT32 ims = e1000R(E1000_IMS);
    if (!(icr & ims)) return;
    g_e1000Irq++;
    if (guestInterruptsEnabled(partition)) {
        injectDeviceIrq(partition, GSI_E1000, 0x7B);
        pendingE1000Irq = 0;
    } else {
        pendingE1000Irq = 1;
    }
}

void deliverPendingE1000Irq(WHV_PARTITION_HANDLE partition) {
    if (pendingE1000Irq && guestInterruptsEnabled(partition)) {
        injectDeviceIrq(partition, GSI_E1000, 0x7B);
        pendingE1000Irq = 0;
    }
}

static void e1000Raise(WHV_PARTITION_HANDLE partition, UINT32 causes) {
    e1000W(E1000_ICR, e1000R(E1000_ICR) | causes);
    e1000MaybeInjectIrq(partition);
}

static void e1000FixChecksums(unsigned char *frame, UINT32 len) {
    UINT16 etype;
    unsigned char *ip;
    UINT32 ihl, tot;
    if (len < 34) return;
    etype = netRd16(frame + 12);
    if (etype != 0x0800) return;
    ip = frame + 14;
    if ((ip[0] >> 4) != 4) return;
    ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl > len) return;
    tot = netRd16(ip + 2);
    if (tot < ihl || 14 + tot > len) tot = len - 14;
    netWr16(ip + 10, 0);
    netWr16(ip + 10, netChecksum(ip, ihl, 0));
    {
        unsigned char proto = ip[9];
        unsigned char *l4 = ip + ihl;
        UINT32 l4Len = tot - ihl;
        UINT32 src = netRd32(ip + 12), dst = netRd32(ip + 16);
        if (proto == 6 && l4Len >= 20) {
            netWr16(l4 + 16, 0);
            netWr16(l4 + 16, netChecksum(l4, l4Len, netL4ChecksumSeed(src, dst, 6, (UINT16)l4Len)));
        } else if (proto == 17 && l4Len >= 8) {
            netWr16(l4 + 6, 0);
            netWr16(l4 + 6, netChecksum(l4, l4Len, netL4ChecksumSeed(src, dst, 17, (UINT16)l4Len)));
        }
    }
}

static void e1000TransmitFrame(WHV_PARTITION_HANDLE partition, unsigned char *frame, UINT32 len) {
    if (len < 14) return;
    if (len < 60) {
        unsigned char pad[60];
        memset(pad, 0, 60);
        memcpy(pad, frame, len);
        frame = pad;
        len = 60;
    }
    e1000FixChecksums(frame, len);
    g_e1000Tx++;
    g_netTxFrames++;
    if (g_e1000Tx <= 8 || g_e1000Tx % 1000 == 0) {
        printf("[e1000] TX #%ld %u bytes etype=0x%04X\n", g_e1000Tx, len, netRd16(frame + 12));
        fflush(stdout);
    }
    if (g_netTransmit) g_netTransmit(partition, frame, len);
}

static void e1000TsoFlush(WHV_PARTITION_HANDLE partition) {
    UINT32 hdr, mss, pay, off, seq0, ident;
    unsigned char *ip, *tcp;
    UINT32 ihl, doff;
    if (!e1000TxTse || e1000TxMss == 0 || e1000TxHdrlen < 40 || e1000TxAccLen <= e1000TxHdrlen) {
        e1000TransmitFrame(partition, e1000TxAcc, e1000TxAccLen);
        return;
    }
    hdr = e1000TxHdrlen;
    mss = e1000TxMss;
    if (hdr >= e1000TxAccLen || hdr + 14 > E1000_TX_BUF_MAX) {
        e1000TransmitFrame(partition, e1000TxAcc, e1000TxAccLen);
        return;
    }
    ip = e1000TxAcc + 14;
    ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl + 20 > hdr) {
        e1000TransmitFrame(partition, e1000TxAcc, e1000TxAccLen);
        return;
    }
    tcp = ip + ihl;
    doff = (tcp[12] >> 4) * 4;
    if (doff < 20) {
        e1000TransmitFrame(partition, e1000TxAcc, e1000TxAccLen);
        return;
    }
    pay = e1000TxAccLen - hdr;
    seq0 = netRd32(tcp + 4);
    ident = netRd16(ip + 4);
    for (off = 0; off < pay; ) {
        UINT32 chunk = pay - off;
        unsigned char seg[2048];
        UINT32 seglen;
        if (chunk > mss) chunk = mss;
        seglen = hdr + chunk;
        if (seglen > sizeof(seg)) break;
        memcpy(seg, e1000TxAcc, hdr);
        memcpy(seg + hdr, e1000TxAcc + hdr + off, chunk);
        netWr16(seg + 14 + 2, (UINT16)(ihl + doff + chunk));
        netWr16(seg + 14 + 4, (UINT16)(ident + (off / mss)));
        netWr32(seg + 14 + ihl + 4, seq0 + off);
        if (off + chunk < pay) seg[14 + ihl + 13] &= (unsigned char)~0x01; // FIN only on last
        e1000TransmitFrame(partition, seg, seglen);
        off += chunk;
    }
}

static void e1000TxFlushEop(WHV_PARTITION_HANDLE partition, int tse) {
    if (e1000TxAccLen == 0) return;
    if (tse) e1000TsoFlush(partition);
    else e1000TransmitFrame(partition, e1000TxAcc, e1000TxAccLen);
    e1000TxAccLen = 0;
}

static void e1000AppendTx(const unsigned char *src, UINT32 n) {
    if (n == 0) return;
    if (e1000TxAccLen + n > E1000_TX_BUF_MAX) n = E1000_TX_BUF_MAX - e1000TxAccLen;
    memcpy(e1000TxAcc + e1000TxAccLen, src, n);
    e1000TxAccLen += n;
}

static void e1000ProcessTx(WHV_PARTITION_HANDLE partition) {
    UINT32 tdlen = e1000R(E1000_TDLEN);
    UINT32 n = tdlen / 16;
    UINT32 tdh, tdt, baseLo, baseHi;
    UINT64 base;
    int needIrq = 0;
    if (!(e1000R(E1000_TCTL) & E1000_TCTL_EN) || n == 0) return;
    tdh = e1000R(E1000_TDH);
    tdt = e1000R(E1000_TDT);
    baseLo = e1000R(E1000_TDBAL) & ~0xFu;
    baseHi = e1000R(E1000_TDBAH);
    base = ((UINT64)baseHi << 32) | baseLo;
    while (tdh != tdt && tdh < n) {
        unsigned char desc[16];
        UINT64 buf;
        UINT32 lower, upper;
        UINT8 cmd, sta;
        UINT32 length;
        int dext;
        if (!e1000GuestCopy(base + (UINT64)tdh * 16, desc, 16, 0)) break;
        buf = (UINT64)desc[0] | ((UINT64)desc[1] << 8) | ((UINT64)desc[2] << 16) | ((UINT64)desc[3] << 24)
            | ((UINT64)desc[4] << 32) | ((UINT64)desc[5] << 40) | ((UINT64)desc[6] << 48) | ((UINT64)desc[7] << 56);
        lower = (UINT32)desc[8] | ((UINT32)desc[9] << 8) | ((UINT32)desc[10] << 16) | ((UINT32)desc[11] << 24);
        upper = (UINT32)desc[12] | ((UINT32)desc[13] << 8) | ((UINT32)desc[14] << 16) | ((UINT32)desc[15] << 24);
        cmd = (UINT8)(lower >> 24);
        dext = (cmd & E1000_TXD_CMD_DEXT) ? 1 : 0;
        if (!dext) {
            length = lower & 0xFFFF;
            cmd = desc[11];
        } else {
            UINT32 dtyp = (lower >> 20) & 0xF;
            length = lower & 0xFFFFF;
            if (dtyp == 0) {
                // Context descriptor: capture TSO / header length.
                e1000TxHdrlen = (upper >> 8) & 0xFF;
                e1000TxMss = (upper >> 16) & 0xFFFF;
                e1000TxTse = (cmd & 0x04) ? 1 : 0;
                length = 0;
            }
        }
        if (length && buf) {
            unsigned char tmp[4096];
            UINT32 left = length, got = 0;
            while (left) {
                UINT32 chunk = left > sizeof(tmp) ? (UINT32)sizeof(tmp) : left;
                if (!e1000GuestCopy(buf + got, tmp, chunk, 0)) break;
                e1000AppendTx(tmp, chunk);
                left -= chunk;
                got += chunk;
            }
        }
        if (cmd & E1000_TXD_CMD_EOP) {
            e1000TxFlushEop(partition, e1000TxTse || (dext && (cmd & 0x04)));
        }
        sta = E1000_TXD_STAT_DD;
        desc[12] = sta;
        e1000GuestCopy(base + (UINT64)tdh * 16, desc, 16, 1);
        if (cmd & E1000_TXD_CMD_RS) needIrq = 1;
        tdh = (tdh + 1) % n;
    }
    e1000W(E1000_TDH, tdh);
    if (needIrq) e1000Raise(partition, E1000_ICR_TXDW);
    if (tdh == tdt) e1000Raise(partition, E1000_ICR_TXQE);
}

void e1000ReceiveFrame(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    UINT32 n, rdh, rdt, rctl, bufsz;
    UINT64 base;
    unsigned char padded[2048];
    UINT32 writeLen, status;
    unsigned char desc[16];
    UINT64 buf;
    if (!e1000RxEnabled() || !frame) return;
    n = e1000RxDescCount();
    if (n == 0) { g_e1000RxDrop++; return; }
    rdh = e1000R(E1000_RDH);
    rdt = e1000R(E1000_RDT);
    if (rdh >= n || rdt >= n) { g_e1000RxDrop++; return; }
    if (((rdt + n - rdh) % n) <= 1) { g_e1000RxDrop++; e1000Raise(partition, E1000_ICR_RXO); return; }
    rctl = e1000R(E1000_RCTL);
    bufsz = 2048;
    if (rctl & 0x00010000) bufsz = 1024;
    if (rctl & 0x00020000) bufsz = 512;
    if ((rctl & 0x00030000) == 0x00030000) bufsz = 256;
    if (rctl & 0x02000000) { // BSEX
        if (bufsz == 2048) bufsz = 16384;
        else if (bufsz == 1024) bufsz = 8192;
        else if (bufsz == 512) bufsz = 4096;
    }
    writeLen = len;
    if (writeLen < 60) {
        memset(padded, 0, 60);
        memcpy(padded, frame, writeLen);
        frame = padded;
        writeLen = 60;
    }
    if (!(rctl & E1000_RCTL_SECRC)) {
        if (writeLen + 4 > sizeof(padded)) { g_e1000RxDrop++; return; }
        if (frame != padded) {
            memcpy(padded, frame, writeLen);
            frame = padded;
        }
        padded[writeLen] = padded[writeLen + 1] = padded[writeLen + 2] = padded[writeLen + 3] = 0;
        writeLen += 4;
    }
    if (writeLen > bufsz) { g_e1000RxDrop++; return; }
    base = ((UINT64)e1000R(E1000_RDBAH) << 32) | (e1000R(E1000_RDBAL) & ~0xFu);
    if (!e1000GuestCopy(base + (UINT64)rdh * 16, desc, 16, 0)) { g_e1000RxDrop++; return; }
    buf = (UINT64)desc[0] | ((UINT64)desc[1] << 8) | ((UINT64)desc[2] << 16) | ((UINT64)desc[3] << 24)
        | ((UINT64)desc[4] << 32) | ((UINT64)desc[5] << 40) | ((UINT64)desc[6] << 48) | ((UINT64)desc[7] << 56);
    if (!buf || !e1000GuestCopy(buf, (void *)frame, writeLen, 1)) { g_e1000RxDrop++; return; }
    status = E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP;
    desc[8] = (unsigned char)(writeLen & 0xFF);
    desc[9] = (unsigned char)((writeLen >> 8) & 0xFF);
    desc[10] = desc[11] = 0;
    desc[12] = (unsigned char)status;
    desc[13] = 0;
    desc[14] = desc[15] = 0;
    e1000GuestCopy(base + (UINT64)rdh * 16, desc, 16, 1);
    e1000W(E1000_RDH, (rdh + 1) % n);
    g_e1000Rx++;
    e1000Raise(partition, E1000_ICR_RXT0);
}

static UINT32 e1000RegRead(UINT32 off) {
    off &= ~3u;
    off &= (E1000_MMIO_SIZE - 1);
    switch (off) {
        case E1000_ICR: {
            UINT32 v = e1000R(E1000_ICR);
            e1000W(E1000_ICR, 0); // 82540: read clears
            return v;
        }
        case E1000_STATUS:
            return e1000R(E1000_STATUS) | E1000_STATUS_LU | E1000_STATUS_FD | E1000_STATUS_SPEED_1000;
        default:
            return e1000R(off);
    }
}

static void e1000RegWrite(WHV_PARTITION_HANDLE partition, UINT32 off, UINT32 val) {
    off &= ~3u;
    off &= (E1000_MMIO_SIZE - 1);
    switch (off) {
        case E1000_CTRL:
            if (val & (E1000_CTRL_RST | E1000_CTRL_PHY_RST)) {
                e1000Reset();
                e1000Raise(partition, E1000_ICR_LSC);
                return;
            }
            e1000W(off, val & ~(E1000_CTRL_RST | E1000_CTRL_PHY_RST));
            break;
        case E1000_STATUS:
            break; // read-only
        case E1000_EERD:
            if (val & 1) {
                UINT32 addr = (val >> 8) & 0xFF;
                UINT16 data = (addr < E1000_EEPROM_WORDS) ? e1000Eeprom[addr] : 0;
                e1000W(E1000_EERD, (val & ~1u) | ((UINT32)data << 16) | 0x10);
            } else {
                e1000W(E1000_EERD, val);
            }
            break;
        case E1000_MDIC: {
            UINT32 op = (val >> 26) & 3;
            UINT32 reg = (val >> 16) & 0x1F;
            UINT32 data = val & 0xFFFF;
            if (op == 1) { // write
                if (reg < 32) e1000Phy[reg] = (UINT16)data;
            } else if (op == 2) { // read
                data = (reg < 32) ? e1000Phy[reg] : 0;
            }
            e1000W(E1000_MDIC, (val & 0x0FFF0000u) | (data & 0xFFFFu) | 0x10000000u);
            break;
        }
        case E1000_ICR:
            e1000W(E1000_ICR, e1000R(E1000_ICR) & ~val); // write-1-to-clear also accepted
            e1000MaybeInjectIrq(partition);
            break;
        case E1000_ICS:
            e1000Raise(partition, val);
            break;
        case E1000_IMS:
            e1000W(E1000_IMS, e1000R(E1000_IMS) | val);
            e1000MaybeInjectIrq(partition);
            break;
        case E1000_IMC:
            e1000W(E1000_IMS, e1000R(E1000_IMS) & ~val);
            break;
        case E1000_TDT:
            e1000W(off, val);
            e1000ProcessTx(partition);
            break;
        case E1000_RCTL:
            e1000W(off, val);
            break;
        default:
            e1000W(off, val);
            break;
    }
}

void e1000HandleBar0Access(WHV_X64_IO_PORT_ACCESS_CONTEXT *io, UINT32 baseOffset, UINT32 accessSize, UINT64 *rax) {
    if (io->AccessInfo.IsWrite) {
        if (baseOffset == 0x10 && accessSize >= 4) {
            UINT32 written = (UINT32)io->Rax;
            if (written == 0xFFFFFFFF) {
                e1000Bar0Sizing = 1;
            } else {
                e1000Bar0Sizing = 0;
                UINT32 newBase = written & ~(UINT32)(E1000_MMIO_SIZE - 1);
                if (newBase != 0 && newBase != e1000MmioBase) {
                    e1000MmioBase = newBase;
                    e1000BarMapped = 1;
                    printf("[e1000] MMIO BAR0 at 0x%X (%u KB, unmapped/trapped)\n",
                           newBase, E1000_MMIO_SIZE / 1024);
                    fflush(stdout);
                }
            }
        }
        return;
    }
    {
        UINT32 currentValue = e1000Bar0Sizing ? (UINT32)(~(UINT32)(E1000_MMIO_SIZE - 1))
                                              : (e1000MmioBase & ~(UINT32)(E1000_MMIO_SIZE - 1));
        UINT32 shift = (baseOffset - 0x10) * 8;
        UINT64 mask = (accessSize >= 4) ? 0xFFFFFFFFULL : (accessSize >= 2 ? 0xFFFFULL : 0xFFULL);
        *rax = (currentValue >> shift) & mask;
    }
}

void e1000HandleBar1Access(WHV_X64_IO_PORT_ACCESS_CONTEXT *io, UINT32 baseOffset, UINT32 accessSize, UINT64 *rax) {
    if (io->AccessInfo.IsWrite) {
        if (baseOffset == 0x14 && accessSize >= 4) {
            UINT32 written = (UINT32)io->Rax;
            if (written == 0xFFFFFFFF) {
                e1000Bar1Sizing = 1;
            } else {
                e1000Bar1Sizing = 0;
                UINT32 addrPart = written & ~(UINT32)(E1000_IO_SIZE - 1);
                if (addrPart != 0) {
                    e1000IoBase = addrPart | 0x1;
                    printf("[e1000] I/O BAR1 at 0x%X\n", e1000IoBase & ~3u);
                    fflush(stdout);
                }
            }
        }
        return;
    }
    {
        UINT32 currentValue = e1000Bar1Sizing ? (~(UINT32)(E1000_IO_SIZE - 1) | 0x1) : e1000IoBase;
        UINT32 shift = (baseOffset - 0x14) * 8;
        UINT64 mask = (accessSize >= 4) ? 0xFFFFFFFFULL : (accessSize >= 2 ? 0xFFFFULL : 0xFFULL);
        *rax = (currentValue >> shift) & mask;
    }
}

int e1000HandleMmio(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    UINT64 gpa = exitContext->MemoryAccess.Gpa;
    UINT32 off;
    int isWrite = 0, isImm = 0, regNum = 0, insnTotalLen = 0;
    UINT32 immVal = 0;
    if (!e1000BarMapped) return 0;
    if (gpa < e1000MmioBase || gpa >= (UINT64)e1000MmioBase + E1000_MMIO_SIZE) return 0;
    off = (UINT32)(gpa - e1000MmioBase);

    if (!ioapicDecodeMmio(exitContext->MemoryAccess.InstructionBytes,
                          exitContext->MemoryAccess.InstructionByteCount,
                          &isWrite, &isImm, &regNum, &immVal, &insnTotalLen)) {
        static int failLog = 0;
        if (failLog++ < 20) {
            printf("[e1000-mmio] undecodable insn rip=0x%llX off=0x%X\n",
                   (unsigned long long)exitContext->VpContext.Rip, off);
            fflush(stdout);
        }
        return 0;
    }

    if (isWrite) g_e1000MmioWrites++; else g_e1000MmioReads++;

    if (isWrite) {
        UINT32 value = immVal;
        if (!isImm) {
            WHV_REGISTER_VALUE srcVal = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &srcVal);
            value = (UINT32)srcVal.Reg64;
        }
        e1000RegWrite(partition, off, value);
        if (g_e1000MmioWrites <= 40) {
            printf("[e1000] W off=0x%X val=0x%08X\n", off, value);
            fflush(stdout);
        }
    } else {
        UINT32 value = e1000RegRead(off);
        WHV_REGISTER_VALUE dstVal = { 0 };
        dstVal.Reg64 = value;
        WHvSetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &dstVal);
        if (g_e1000MmioReads <= 40) {
            printf("[e1000] R off=0x%X -> 0x%08X\n", off, value);
            fflush(stdout);
        }
    }

    {
        WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
        WHV_REGISTER_VALUE ripVal = { 0 };
        ripVal.Reg64 = exitContext->VpContext.Rip + (UINT64)insnTotalLen;
        WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &ripVal);
    }
    return 1;
}

void e1000HandleIoAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;
    UINT32 base = e1000IoBase & ~3u;
    UINT32 off = (UINT32)io->PortNumber - base;
    UINT32 accessSize = io->AccessInfo.AccessSize ? io->AccessInfo.AccessSize : 4;
    UINT64 rax = io->Rax;

    if (off < 4) {
        if (io->AccessInfo.IsWrite) e1000IoAddr = (UINT32)rax & (E1000_MMIO_SIZE - 1);
        else rax = e1000IoAddr;
    } else {
        UINT32 mmioOff = e1000IoAddr & (E1000_MMIO_SIZE - 1);
        if (io->AccessInfo.IsWrite) e1000RegWrite(partition, mmioOff, (UINT32)rax);
        else rax = e1000RegRead(mmioOff);
        (void)accessSize;
    }

    {
        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
        WHV_REGISTER_VALUE values[2] = { 0 };
        values[0].Reg64 = rax;
        values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
    }
}
