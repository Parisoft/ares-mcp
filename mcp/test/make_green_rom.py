#!/usr/bin/env python3
"""
Generate a minimal N64 test ROM for the ares-mcp headless host.

Layout (big-endian .z64, 4 MiB) — the libdragon ipl3 image is kept INTACT:
  0x0000-0x003F  header from libdragon's ipl3_prod.z64 (CIC-6102)
  0x0040-0x1147  libdragon's signed IPL3 trampoline, copied verbatim.
  0x1148          0x00004000 — entry-offset word: the CPU lands at
                 RDRAM 0x80004000 (PIF HLE bootLoadGame) and falls into code.
  0x114C-0x2147   our "game" (runs from RDRAM 0x80004004):
                   sets SP, fills a 640x480x4 framebuffer with green plus a
                   white box, flushes the dcache, initializes the VI
                   (ares register map), then idles forever.

MIPS register numbers (standard, verified against ares/n64/cpu/decoder.cpp):
  zero=0  a0=4  a1=5  a2=6  a3=7  t0=8  t1=9  t3=11  sp=29

ares N64 opcode facts (non-standard SPECIAL table!):
  ADDU = SPECIAL fn 0x21   (0x2A is SLT in ares)
  CACHE op 0x2F: operation = bits 20-16, offset = bits 15-0, base = rs
  dcache ops: 0x11 = hit invalidate, 0x15 = hit writeback+invalidate

Usage: python3 make_green_rom.py [seed.bin] [out.z64]
"""
import struct
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
IPL3 = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "ipl3_seed.bin")
OUT = sys.argv[2] if len(sys.argv) > 2 else "green.z64"
ROM_SIZE = 4 * 1024 * 1024
ROM_BASE = 0x1148
RAM_BASE = 0x80004000      # game runs here in RDRAM (KSEG0)

# register aliases (standard MIPS numbering)
ZERO, AT, V0, V1 = 0, 1, 2, 3
A0, A1, A2, A3 = 4, 5, 6, 7
T0, T1, T2, T3 = 8, 9, 10, 11
SP = 29

base = open(IPL3, "rb").read()
assert len(base) >= ROM_BASE + 4, "seed file too small"

# ---------------------------------------------------------------- assembler
class A:
    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
    def _emit(self, w):
        self.words.append(w & 0xFFFFFFFF)
    @property
    def pc(self):
        return self.base + 4 * len(self.words)
    def label(self, name):
        self.labels[name] = self.base + 4 * len(self.words)
    def nop(self): self._emit(0)
    def lui(self, rt, imm): self._emit(0x3C000000 | rt << 16 | (imm & 0xFFFF))
    def ori(self, rt, rs, imm): self._emit(0x34000000 | rs << 21 | rt << 16 | (imm & 0xFFFF))
    def li(self, rt, val):
        val &= 0xFFFFFFFF
        hi, lo = val >> 16, val & 0xFFFF
        if lo & 0x8000:
            self.lui(rt, (hi + 1) & 0xFFFF)
            self.addiu(rt, rt, lo - 0x10000)
        else:
            self.lui(rt, hi)
            if lo: self.ori(rt, rt, lo)
    def addiu(self, rt, rs, imm):
        imm &= 0xFFFF
        if imm & 0x8000: imm -= 0x10000
        assert -0x8000 <= imm < 0x8000, f"addiu immediate {imm:#x} out of range"
        self._emit(0x24000000 | rs << 21 | rt << 16 | (imm & 0xFFFF))
    def addu(self, rd, rs, rt):  # rd = rs + rt (ares SPECIAL fn 0x21)
        self._emit(0x00000000 | rs << 21 | rt << 16 | rd << 11 | 0x21)
    # ares SLL: destination = RD field, source = RT field (non-standard!)
    def move(self, rt, rs): self._emit(0x00000000 | rs << 16 | rt << 11)
    def sw(self, rt, off, rs): self._emit(0xAC000000 | rs << 21 | rt << 16 | (off & 0xFFFF))
    def lw(self, rt, off, rs): self._emit(0x8C000000 | rs << 21 | rt << 16 | (off & 0xFFFF))
    def bne(self, rs, rt, label):
        target = self.labels[label]
        off = (target - (self.pc + 4)) >> 2   # branch PC is pc+4
        assert -0x20000 < off < 0x20000, "bne out of range"
        self._emit(0x14000000 | rs << 21 | rt << 16 | (off & 0xFFFF))
    def j(self, target):
        if isinstance(target, str): target = self.labels[target]
        field = (target & 0x3FFFFFFF) >> 2
        assert (target >> 28) == (self.pc >> 28), "j target leaves 256 MiB region"
        self._emit(0x08000000 | field)
    def cache(self, op, off, rs):  # ares: operation = bits 20-16, offset = bits 15-0
        self._emit(0xBC000000 | rs << 21 | (op & 0x1F) << 16 | (off & 0xFFFF))

a = A(RAM_BASE + 4)   # code at ROM 0x114C runs from RDRAM 0x80004004

a.li(SP, 0x803F0000)              # sp into plain RDRAM

# ---- fill framebuffer 0x80100000..0x8012C000 (640x480x4, 1.125 MiB) green
a.li(A0, 0x80100000)              # start
a.li(A1, 0x8012C000)              # end
a.li(A2, 0x00FF0000)              # green
a.label("fill")
a.sw(A2, 0, A0)
a.addiu(A0, A0, 4)
a.bne(A0, A1, "fill")
a.nop()

# ---- white box: 120 rows x 240 cols starting at (x=200, y=180)
a.li(A0, 0x80100000)              # row base
a.li(T1, 180 * 2560)              # y = 180 rows down (0x70800)
a.addu(A0, A0, T1)
a.li(A1, 120)                     # rows
a.li(A3, 0xFFFFFFFF)              # white (VI maps 24bpp as data>>8)
a.label("box_y")
a.move(A2, A0)
a.addiu(A2, A2, 200 * 4)          # x = 200 cols in
a.li(T0, 240)                     # cols
a.label("box_x")
a.sw(A3, 0, A2)
a.addiu(A2, A2, 4)
a.addiu(T0, T0, -1)
a.bne(T0, ZERO, "box_x")
a.nop()
a.addiu(A0, A0, 2560)             # next row (640 px * 4 bytes)
a.addiu(A1, A1, -1)
a.bne(A1, ZERO, "box_y")
a.nop()

# ---- dcache writeback+invalidate 0x80100000..0x8012C000 (16-byte lines)
a.li(A0, 0x80100000)
a.li(A1, 0x8012C000)
a.label("dcache")
a.cache(0x15, 0, A0)              # hit writeback+invalidate
a.cache(0x11, 0, A0)              # hit invalidate
a.addiu(A0, A0, 16)
a.bne(A0, A1, "dcache")
a.nop()

# ---- VI init (ares register map: ares/n64/vi/io.cpp)
a.li(A0, 0xA4400000)              # VI base (physical 0x04400000)
a.li(A1, 3)                       # VI_CONTROL: colorDepth=3 (24bpp)
a.sw(A1, 0x00, A0)
a.li(A1, 0x00100000)              # VI_DRAM_ADDRESS
a.sw(A1, 0x04, A0)
a.li(A1, 640)                     # VI_H_WIDTH
a.sw(A1, 0x08, A0)
a.li(A1, 256)                     # VI_V_INTR (coincidence)
a.sw(A1, 0x0C, A0)
a.li(A1, 264)                     # VI_V_TOTAL: half-lines per field
a.sw(A1, 0x18, A0)
a.li(A1, 1535)                    # VI_H_TOTAL: quarter-line duration -> ~60 Hz
a.sw(A1, 0x1C, A0)
a.li(A1, 0x00460046)              # VI_H_SYNC_LEAP
a.sw(A1, 0x20, A0)
a.li(A1, 0x006C02EC)              # VI_H_VIDEO: hstart=108, hend=748
a.sw(A1, 0x24, A0)
a.li(A1, 0x00220202)              # VI_V_VIDEO: vstart=34, vend=514
a.sw(A1, 0x28, A0)
a.li(A1, 0x000E0071)              # VI_V_BURST
a.sw(A1, 0x2C, A0)
a.li(A1, 1024)                    # VI_X_SCALE: 1:1
a.sw(A1, 0x30, A0)
a.li(A1, 2048)                    # VI_Y_SCALE: 1:1
a.sw(A1, 0x34, A0)

# ---- idle (the VI keeps scanning the framebuffer out of RDRAM)
a.label("idle")
a.nop()
a.j("idle")
a.nop()

code = b"".join(struct.pack(">I", w) for w in a.words)
assert len(code) <= 0x1000, "game code too big"

# ------------------------------------------------------------------ assemble
rom = bytearray(ROM_SIZE)
rom[0x0000:0x1148] = base[0x0000:0x1148]  # header + full ipl3 trampoline
struct.pack_into(">I", rom, 0x1148, 0x00004000)  # entry = 0x4000
rom[0x114C:0x114C + len(code)] = code

open(OUT, "wb").write(bytes(rom))
print(f"wrote {OUT}: {len(rom)} bytes, game = {len(a.words)} instructions "
      f"@ROM 0x114C (runs @RDRAM 0x80004004)")
