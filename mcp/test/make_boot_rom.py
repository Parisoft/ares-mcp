#!/usr/bin/env python3
"""
Generate the minimal PIF boot ROM (pif.ntsc.rom) for the ares-mcp host.

Replaces the genuine IPL2. The ROM runs from 0xBFC00000 (KSEG1 PIF window;
ares routes paddr 0x1FC00000-0x1FC007FF to the PIF).

  Phase 1 (ROM[0x00..0x3B], runs from PIF ROM): copies the phase-2 payload
  (ROM[0x3C..]) to RSP DMEM @0xA4000000 and jumps to it. Required because
  the ares PIF HLE sets romLockout on the 0x10 command, after which PIF ROM
  reads (including instruction fetch) return 0.
  Phase 2 (ROM[0x3C..], runs from 0xA4000000): drives the ares PIF HLE
  handshake — 0x10 twice (the first is consumed by the HLE Init state),
  writes the CIC-NUS-7101/6102 checksum constant a5 36 c0 f1 d8 59 to
  PIF RAM[0x32..0x37] (abs 0x7F2..0x7F7), 0x20, 0x40 (strict checksum
  verify), 0x08 (-> Run) — then jumps to
  RDRAM[0x80000000 + (cartridge[0x1148] & 0x1FFFF)].

Output: 1984 bytes (0x7C0), zero-padded.
"""
import struct, sys

REG = {n: i for i, n in enumerate(
    "zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 "
    "s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 t10 t11 k0 k1 gp sp".split())}
def r(n): return REG[n]

class A:
    def __init__(self, base=0):
        self.words, self.labels = [], {}
    def emit(self, w): self.words.append(w & 0xFFFFFFFF)
    @property
    def pc(self): return 4 * len(self.words)
    def label(self, name): self.labels[name] = self.pc
    def nop(self): self.emit(0)
    def lui(self, rt, imm): self.emit(0x3C000000 | r(rt) << 16 | (imm & 0xFFFF))
    def addiu(self, rt, rs, imm):
        imm &= 0xFFFF
        if imm & 0x8000: imm -= 0x10000
        self.emit(0x24000000 | r(rs) << 21 | r(rt) << 16 | (imm & 0xFFFF))
    def li(self, rt, val):
        val &= 0xFFFFFFFF
        hi, lo = val >> 16, val & 0xFFFF
        if lo & 0x8000:
            self.lui(rt, (hi + 1) & 0xFFFF)
            self.addiu(rt, rt, lo - 0x10000)
        else:
            if hi: self.lui(rt, hi)
            if lo: self.addiu(rt, "zero", lo)
    def lw(self, rt, off, rs): self.emit(0x8C000000 | r(rs) << 21 | r(rt) << 16 | (off & 0xFFFF))
    def sw(self, rt, off, rs): self.emit(0xAC000000 | r(rs) << 21 | r(rt) << 16 | (off & 0xFFFF))
    def sb(self, rt, off, rs): self.emit(0xA0000000 | r(rs) << 21 | r(rt) << 16 | (off & 0xFFFF))
    def andi(self, rt, rs, imm): self.emit(0x30000000 | r(rs) << 21 | r(rt) << 16 | (imm & 0xFFFF))
    def addu(self, rd, rs, rt): self.emit(0x00000000 | r(rs) << 21 | r(rt) << 16 | r(rd) << 11 | 0x21)
    def bne(self, rs, rt, label):
        target = self.labels[label]
        off = (target - (self.pc + 4)) >> 2
        assert -0x20000 < off < 0x20000
        self.emit(0x14000000 | r(rs) << 21 | r(rt) << 16 | (off & 0xFFFF))
    def jr(self, rs): self.emit(0x00000000 | r(rs) << 21 | 0x08)
    def words_bytes(self):
        return b"".join(struct.pack(">I", w) for w in self.words)

# ------------------------------------------------------------------ phase 1
a = A()
a.lui("t1", 0xBFC0)          # src base (PIF ROM, KSEG1)
a.addiu("t1", "t1", 0)       # patched below: src = ROM[phase 2 start]
a.lui("t2", 0xA400)             # dst base (RSP DMEM, CPU side)
a.addiu("t2", "t2", 0x00)
a.addiu("t3", "zero", 0)      # patched below: li t3, N
a.label("copy")
a.lw("t0", 0, "t1")
a.sw("t0", 0, "t2")
a.addiu("t1", "t1", 4)
a.addiu("t2", "t2", 4)
a.addiu("t3", "t3", -1)
a.bne("t3", "zero", "copy")
a.nop()                         # delay slot
a.lui("t1", 0xA400)             # jump target = 0xA4000000
a.jr("t1")
a.nop()                         # delay slot
PAYLOAD_OFF = a.pc

# ------------------------------------------------------------------ phase 2
# Debug canary at 0xA0002000 (kssseg, uncached alias of RDRAM 0x2000):
# progress marker visible in rdram.ram.data[0x2000] (KSEG0 stores would
# park in the D-cache and be invisible to the host-side sampler).
def canary(n):
    b.li("t5", n)
    b.sw("t5", 0, "s0")
b = A()
b.lui("s0", 0xA000)             # canary base
b.addiu("s0", "s0", 0x2000)     # 0xA0002000
b.lui("at", 0xBFC0)             # PIF window (KSEG1)
b.li("t0", 0x10)
b.sb("t0", 0x7FF, "at")         # 1st 0x10: consumed by HLE Init state
canary(1)
b.li("t0", 0x10)
b.sb("t0", 0x7FF, "at")         # 2nd 0x10: lockout + PIF::bootLoadGame()
canary(2)
# CIC-NUS-7101/6102 checksum into PIF RAM[0x32..0x37] (window 0x7F2..0x7F7).
# Byte stores only: ares raises an address-store exception on unaligned sw.
for off, val in ((0x7F2, 0xA5), (0x7F3, 0x36), (0x7F4, 0xC0),
                 (0x7F5, 0xF1), (0x7F6, 0xD8), (0x7F7, 0x59)):
    b.li("t0", val)
    b.sb("t0", off, "at")
canary(3)
b.li("t0", 0x20)
b.sb("t0", 0x7FF, "at")         # HLE stashes the CPU checksum
canary(4)
b.li("t0", 0x40)
b.sb("t0", 0x7FF, "at")         # HLE verifies checksum (CIC-NUS-7101: a536c0f1d859)
canary(5)
b.li("t0", 0x08)
b.sb("t0", 0x7FF, "at")         # HLE -> Run
canary(6)
canary(7)
b.lui("t1", 0xB000)             # KSEG1 cartridge
b.lw("t0", 0x1148, "t1")        # ipl3 entry-offset word
b.andi("t0", "t0", 0xFFFF)      # 16-bit entry offset
b.lui("t3", 0x8000)
b.addu("t0", "t3", "t0")        # 0x80000000 + entry
b.jr("t0")
b.nop()
N = len(b.words)

# patch the copy source offset + count into phase 1
a.words[1] = 0x24000000 | r("t1") << 21 | r("t1") << 16 | (PAYLOAD_OFF & 0xFFFF)
a.words[4] = 0x24000000 | 0 << 21 | r("t3") << 16 | (N & 0xFFFF)

rom = a.words_bytes() + b.words_bytes()
assert rom[PAYLOAD_OFF:PAYLOAD_OFF + 4] == b"\x3c\x10\xa0\x00", "phase 2 must start at its offset"
rom += b"\x00" * (0x7C0 - len(rom))
assert len(rom) == 0x7C0

# ---------------------------------------------------------------- verify
def name(i): return [k for k, v in REG.items() if v == i][0]
def check(rom, expected_words, tag, offset=0):
    bad = 0
    for i in range(len(expected_words)):
        w = struct.unpack_from(">I", rom, offset + i * 4)[0]
        if w != expected_words[i]:
            bad += 1
            print(f"  MISMATCH {tag}[{i*4:02x}]: got {w:08x}, want {expected_words[i]:08x}")
    print(f"verify {tag}: {len(expected_words)} words, {bad} mismatches")
    assert bad == 0
check(rom, a.words, "phase1")
check(rom, b.words, "phase2", offset=PAYLOAD_OFF)

out = sys.argv[1] if len(sys.argv) > 1 else "boot_rom.bin"
open(out, "wb").write(rom)
print(f"wrote {out}: {len(rom)} bytes (phase1={a.pc // 4} words @0x00, phase2={N} words @0x3C)")
