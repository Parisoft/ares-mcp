# Plan: Headless N64 MCP Server for Ares

**Goal:** strip the frontend (SDL, hiro GUI, desktop-ui, audio/video devices, X server) from this
fork and ship a **Model Context Protocol (MCP) server** that hosts the ares N64 core headlessly,
so AI agents can load a ROM, run it, debug it (breakpoints, watchpoints, step, registers), read
memory/VRAM, capture screenshots and WAV audio, and inspect disassembly — for validating N64
games under development in CI-style environments with no display.

No code changes are made by this document; it is a build/implementation plan with a full tool
catalog. All file references below were verified against the current tree.

---

## 1. What the code already gives us (survey results)

### 1.1 Layered architecture

| Layer | Dir | Role | Needed by MCP? |
|---|---|---|---|
| `nall` | `nall/` | "C++ stdlib": strings, files, VFS, image encode/decode (PNG, WAV, BMP…), threads, TCP sockets, **GDB remote-protocol server**, main/arguments | **Yes** (no display APIs used by our paths) |
| `libco`, `sljit` | `libco/`, `thirdparty/sljit*` | cooperative coroutines (other cores' scheduler), dynamic recompiler | Yes (recompiler is the default CPU/RSP backend) |
| `ares` | `ares/ares/` | core framework: node tree, `Core::System` (`run/power/serialize/unserialize`), `Platform` interface, debug/trace API | **Yes** |
| N64 core | `ares/n64/` | the emulation core | **Yes** |
| `mia` | `mia/` | ROM/image loader + database (`mia::Medium`, `mia::System`, paks) | **Yes** |
| `ruby` | `ruby/` | SDL audio/video/input, OpenGL/Metal/D3D9 output | **No** — remove |
| `hiro` | `hiro/` | native GUI toolkit (GTK/Qt/Cocoa/Win) | **No** — remove |
| `desktop-ui` | `desktop-ui/` | the `ares` GUI application | **No** — remove (but mine for the load flow, see §3) |
| `thirdparty` GL/KHR/TZXFile/librashader/ymfm/chdr | `thirdparty/` | shaders, CD, other cores | Not for N64-only (chdr is optional) |

Key facts that make this feasible with **little or no core modification**:

1. **The core is already headless-capable.** `ares::Nintendo64::load(root, name)` builds the node
   tree; `root->run()` executes exactly one frame of simulation (the CPU main loop
   `ares/n64/cpu/cpu.cpp` spins `while(!vi.refreshed && GDB::server.reportPC(...))` and returns at
   VI refresh); `root->power()` boots; `root->serialize()/unserialize()` give full save states.
   Nothing in the core requires a display, audio device, or X.

2. **There is a complete GDB Remote Serial Protocol (RSP) server** in
   `nall/nall/gdb/server.{hpp,cpp}` (with a detailed `Readme.md`), built into the N64 core:
   - software breakpoints (Z0), read/write/access watchpoints (Z2/Z1/Z3), single step,
     continue, register read/write (32 GPRs, HI/LO, COP0 cause/status, all 32 FPRs, PC),
     memory read/write, signals/exceptions, `QPassSignals` (last commit on master!),
     PC-override so halts land on the *causing* instruction, recompiler cache invalidation.
   - Hooks are registered in `System::initDebugHooks()` (`ares/n64/system/system.cpp`), and the
     N64 is documented as the *reference implementation* in the GDB server readme.
   - CPU exceptions are mapped to GDB signals (`CPU::Exception::reportGDBException`,
     `ares/n64/cpu/exceptions.cpp`); the recompiler splits basic blocks at breakpoints
     (`ares/n64/cpu/recompiler.cpp`) and tracks watchpoint state.
   - `CPU::main()` calls `GDB::server.reportPC()` **per instruction**, and a
     `Queue::GDB_Poll` event drives `GDB::server.updateLoop()` from inside emulation
     (`ares/n64/cpu/cpu.cpp`), so the server works whether the host is GUI or not.
   - The readme explicitly states the API is usable *without* a GDB client, for "automated
     tooling or custom debugging UIs": plain RSP packets over TCP, optional handshake,
     checksums not verified. → **The MCP server can speak RSP to its own GDB server in-process.**

3. **Neutral, cache-aware memory access for debuggers exists**: `CPU::readDebug<Byte|Half|Word|Dual>`
   and `writeDebug` (`ares/n64/cpu/memory.cpp`) de-virtualize via TLB, honor the dcache, and use
   `RBusDevice::ARES_DEBUGGER` on the RCP bus — reads never perturb the system (no cache flush,
   no timing), writes behave like CPU writes. `cpu.devirtualizeDebug()` maps VA→PA. The GDB
   memory hooks already use these.

4. **Built-in disassemblers with live register values**: `CPU::Disassembler::disassemble(addr, insn)`
   (`ares/n64/cpu/disassembler.cpp`) and `RSP::Disassembler::disassemble(...)`
   (`ares/n64/rsp/disassembler.cpp`) — both render instructions using the *current* register
   contents, exactly what an agent needs.

5. **Software video path (no GPU needed)**: when `vulkan.enable == false` (the "Enable GPU
   acceleration" option, set via `ares::Nintendo64::option(...)`), the VI runs a full software
   processing path — `VI::refresh()` (`ares/n64/vi/vi.cpp`) decodes the 15/24-bit framebuffer from
   RDRAM (VI scaling, viewport, interlace field selection) into the `Screen` pixel buffer. The
   `Screen` node (`ares/ares/node/video/screen.cpp`) runs its own thread (no display needed — the
   `platform->video()` sink is a virtual no-op by default) and calls
   `platform->video(screen, output, pitch, w, h)` with the composited ARGB8888 frame.
   **Screenshot = capture in `Platform::video` (exactly what desktop-ui's `captureScreenshot`
   does), or decode the raw framebuffer from RDRAM.**
   The Vulkan/paraLLEl-RDP path is a *runtime* option: with no Vulkan driver present,
   `Vulkan::load()` fails gracefully and the core falls back (`ares/n64/vulkan/vulkan.cpp`).
   Volk loads `libvulkan` dynamically, so there is **no link-time Vulkan dependency**; with no
   GPU the code simply runs software. (A later phase can add `#ifndef VULKAN` build stripping.)

6. **RDP state is tracked even without a GPU**: with Vulkan off, `RDP::render()`
   (`ares/n64/rdp/render.cpp`) still walks the command stream and updates all RDP state
   (DPC_*/DPS_* registers, TMEM, TLUT, scissor, combine modes) — only pixel rasterization is
   absent. So "read VRAM" (TMEM/TLUT/RDP state) and RDP-command tracing work headless.

7. **Audio is a plain ring buffer**: `Node::Audio::Stream`
   (`ares/ares/node/audio/stream.{hpp,cpp}`) — `write()` from the AI core, `read(f64*)`/`pending()`
   for the consumer, resampler + filters, `frequency()` known. The MCP host drains it to capture
   WAV (`nall::Encode::WAV::stereo<s16>` exists, `nall/encode/wav.hpp`). No audio device needed.
   (Desktop frame *pacing* comes from SDL's blocking audio; headless we pace with a timer or
   drained-sample accounting — §3.3.)

8. **Tracers / debug tree**: every component has `debugger.cpp` registering
   `Node::Debugger::Tracer::Notification/Instruction` nodes (CPU instruction/exception/interrupt/TLB/
   *EMUX*, RDP command/IO, RDRAM IO, PI/SI/MI/VI/AI IO, …) and `Node::Debugger::Memory` views
   (RDRAM + DCache, `ares/n64/rdram/debugger.cpp`). Messages flow to the frontend via
   `Platform::log()`. **EMUX** (`ares/n64/cpu/emux.cpp`) is the homebrew extension set
   (XLOG/XHEXDUMP/XPROF/XIOCTL) — game-side log output, and XIOCTL can even request
   fast-forward or *exit* via `Platform::event()`. For a game-under-development, emux XLOG output
   is gold; the MCP server should surface it as a tool.

9. **ROM loading** is `mia`-based and UI-free: `mia::Medium::create("Nintendo 64")`,
   `medium->load(path)` (parses the raw 0x10-byte N64 header, sets attributes: id, region, cpak/
   rpak/biosensor/tpak per port, dd flag), `mia::System::create("Nintendo 64")->load()` for the
   BIOS paks. The full wiring (cartridge slot port, gamepads, controller paks, options) is in
   `desktop-ui/emulator/nintendo-64.cpp` — that is the template to replicate in ~100 lines without
   any UI. Note: **mia does not handle N64X/Yosh1/Compressed ROMs** (raw header only) → an
   `n64_read_rom`/decompression tool is a real gap we can fill (see §5.5).

10. **`ares::Platform` is a virtual interface with no-op defaults**
    (`ares/ares/platform.hpp`): `attach/detach/pak/event/log/status/video/refreshRateHint/audio/
    input/cheat`. A headless `Platform` subclass is the only "frontend" code the core needs.
    `ares::platform` is a global pointer (`extern Platform* platform`).

11. **Build system already supports N64-only builds**: `ARES_CORES` cache variable
    (root `CMakeLists.txt`) gates each core subdir and defines `CORE_N64`; `desktop-ui` (output
    name `ares`) is a separate subdirectory linking `ares::ruby ares::hiro ares::ares ares::mia
    ares::nall`. SDL/X/GL come only via ruby/hiro (and `deps.json` ares-deps prebuilts). An
    `ARES_BUILD_MCP=ON` configuration that skips `ruby/hiro/desktop-ui/thirdparty-GUI` removes
    every display/audio dependency. `nall::main` (`nall/nall/main.{hpp,cpp}`) provides argument
    parsing + WinMain/WSA glue for a new executable.

12. **JSON**: there is *no* JSON library in the tree (`thirdparty/qon` is a QOI image codec, not
    JSON). The MCP layer needs one — vendoring a single-header MIT library (e.g. nlohmann/json)
    under `thirdparty/` matches the repo's existing vendoring convention and adds no system
    dependencies.

### 1.2 Consequence

The frontend removal is almost purely **build-system + a new leaf target**; the core, its debug
machinery, video software path, audio buffer, and ROM loading are already frontend-agnostic. The
main new code is: a headless `Platform`, a core host (load/pace/save-dir), an MCP JSON-RPC/stdio
server, a small in-process GDB-RSP client, and the tool implementations on top of existing APIs.

---

## 2. Target architecture

```
                 ┌─────────────────────────── ares-mcp (single static binary, stdio) ───────────────────────────┐
 MCP client ────►│  mcp/mcp-server.cpp        stdio JSON-RPC 2.0 (MCP spec)                                        │
 (agent, CI)     │    tools/list / tools/call / initialize                                                         │
                 │  mcp/tools/*.cpp           tool registry + implementations                                      │
                 │  mcp/gdb-client.cpp        in-process RSP client (nall::TCP::Socket ↔ 127.0.0.1:gdb-port)      │
                 │  mcp/core-host.cpp         ROM/BIOS load, frame-loop thread, pacing, pause/resume, save dir     │
                 │  mcp/platform.cpp          ares::Platform impl: log→ring buffer, video→frame grabber,           │
                 │                                              audio→drain hook, event(Shutdown), pak→vfs dir       │
                 └───────┬────────────────────────────────────────────────────────────────────────────────────────┘
                         │ links
        ares (N64 core only, ARES_CORES=n64)  +  mia  +  nall  +  libco  +  sljit
        (nall::GDB::server lives here; CPU/RSP disassemblers, readDebug, tracers, emux,
         VI software path, Stream ring buffer, Screen thread, encode::PNG/WAV)
```

Threads:
- **core thread** — runs `root->run()` per frame (same as desktop-ui's worker thread);
- **MCP I/O thread** — reads stdio, dispatches tools; serializes core access with the pause
  mechanism (§4);
- **screen thread** — pre-existing (`ares::Video::Threaded`), needs no display;
- **GDB server** — pre-existing singleton, TCP on 127.0.0.1 (port configurable, e.g. 20000).
  The MCP gdb-client connects to it in-process; an external `gdb-multiarch` may attach too, but
  the server is single-client, so the MCP "attach external gdb" case and the internal client are
  mutually exclusive by design (documented; see risks).

---

## 3. Headless core host (the new "frontend")

### 3.1 `mcp/platform.cpp` — `ares::Platform` implementation

- `log(tracer, msg)` → append to a ring buffer with (component, name, msg); exposed by
  `n64_log_read`; EMUX/exception/interrupt lines included automatically.
- `status(msg)` → ring buffer + surfaced in `n64_status`.
- `video(screen, data, pitch, w, h)` → copy ARGB8888 into a double-buffered grabber
  (mutex + frame counter). This is the screenshot source (identical to desktop-ui's
  `captureScreenshot` hook in `desktop-ui/program/platform.cpp`).
- `audio(stream)` → mark "audio pending" (the MCP host drains from `ai.stream` directly).
- `event(Event)`: `FastForwardOn/Off` → adjust pacing (homebrew XIOCTL fast-forward),
  `Shutdown` → set game-exited flag (XIOCTL exit; surfaced by `n64_status`).
- `pak(node)` → `vfs::directory` over a local **save folder** (per game: `<save-dir>/<rom-id>/`),
  so controller paks / save files persist between sessions — essential for game development.
- `input(node)` → no-op (input injection is a separate tool, §5.2).

### 3.2 Load flow (replicate `desktop-ui/emulator/nintendo-64.cpp`, minus UI)

1. `ares::platform = &mcpPlatform;`
2. `game = mia::Medium::create("Nintendo 64"); game->load(romPath)` (attributes: id, region,
   cpak/rpak/biosensor/tpak, dd).
3. `ares::Nintendo64::option(...)` for: `Enable GPU acceleration` (default **false**; `true`
   only when a Vulkan driver is present, e.g. lavapipe), `Homebrew Mode`, `Deterministic
   Entropy`, `Recompiler`, `Expansion Pak`, `Quality/Supersampling/...` (no-ops when GPU off),
   `Controller Pak Banks`.
4. `ares::Nintendo64::load(root, "[Nintendo] Nintendo 64 (NTSC|PAL)")` — note this also calls
   `initDebugHooks()`; call `GDB::server.reset()` + `open(port, true)` (127.0.0.1) here, same as
   `desktop-ui/program/load.cpp`.
5. `root->find<Port>("Cartridge Slot")` → `allocate(); connect();`
6. Connect N gamepads (default 1; honor the 3-port cap for ROM id `NJOE`); connect controller
   pak (create `<save-dir>/<id>/save1.pak` 32KiB, `mia::Pak` over it) / rumble pak per ROM
   attributes — save-file persistence comes free.
7. `root->power();`
8. Optional: boot-await-GDB-client behavior (mimic `settings.boot.awaitGDBClient`): power-on but
   hold the frame loop until the MCP host is ready / a breakpoint is set — useful for halting at
   the BIOS entry or first game instruction.

### 3.3 Frame loop + pacing

`core thread: while(running) { if(gdb halted or paused) { update GDB; sleep; } root->run(); pace(); }`

- **Pacing modes** (config, default `realtime`):
  - `realtime` (audio-driven, mirrors desktop): after each frame, sleep so the drained audio
    stays within a latency window (drain count from `ai.stream` / `ai.dac.frequency`). Silent
    games → fall back to 1/60 s (NTSC) or 1/50 s (PAL) frame timer.
  - `timer`: fixed frame period (simplest; drift tolerated for debug use).
  - `fast`: no pacing (batch runs, CI, "run N frames then stop"). With `Deterministic Entropy`
    on, fast runs are **bit-reproducible** — the key CI validation mode.
- **Pause/resume** for tools: atomic flag; the loop waits on a condition variable and, on
  resume, the tool side must first **wait for the in-flight `root->run()` to return**
  (frame-boundary join, ≤1 frame ≈ 16 ms). Instruction-precise halting uses the GDB server
  (breakpoint/step), which stops the CPU mid-frame via `reportPC` and is already race-free
  (same mechanism the GUI uses).
- The run loop mirrors `Program::emulatorRunLoop` (`desktop-ui/program/program.cpp`) including
  its GDB-halt handling (`isHalted()` → only `updateLoop()`), proven in the field.

### 3.4 Save states

`root->serialize(true)` / `root->unserialize(...)` (see the run-ahead trick in
`desktop-ui/program/program.cpp` and `Emulator` in `desktop-ui/emulator/emulator.cpp`):
`n64_save_state`/`n64_load_state` to file (`.state` = ares serializer string) or as an inline
string; also in-memory slots for A/B testing.

---

## 4. Concurrency & safety model

- **One writer at a time.** All state-mutating tools and all reads that need consistency:
  `pause core → join frame → take mcp mutex → operate → resume`.
- **Reads** go through the neutral debugger paths (`cpu.readDebug*`, `bus.read<..,
  ARES_DEBUGGER>`, `devirtualizeDebug`) — verified neutral by design (GDB hooks rely on it), so
  even "live" reads while running don't disturb emulation (same guarantee the GUI debugger has).
- **Writes** go through `cpu.writeDebug*` (cache-flushing, side-effectful like a CPU write) —
  only while paused.
- **GDB RSP client** (MCP internal): a small class over `nall::TCP::Socket` to
  127.0.0.1:<port> speaking RSP packets (`Z0/Z1/Z2/Z3,s,s,c,C,g,G,m,M,p,P,?$,...`). The GDB
  server is non-blocking (`updateLoop()` is pumped by the core thread's `GDB_Poll` queue event
  and by the run loop), so the client's round-trips are serviced even while the core is paused —
  exactly how the readme describes non-GDB automated use.
- **Recompiler caveat (already handled upstream)**: breakpoints force block splitting
  (`recompiler.cpp` uses `GDB::server.hasBreakpointAt()` at block boundaries); watchpoints toggle
  a state-key flag. Keep a `forceInterpreter` option (mirrors `settings.developer.forceInterpreter`)
  for the pathological cases.

---

## 5. MCP tool catalog

Conventions: names prefixed `n64_`; addresses are 64-bit kernel-space VA (`0x80000000+`) by
default, `physical:` / `rcp:` / `cartridge:` prefixes select the raw view; all tools return JSON
with `ok/error`; large payloads (memory, screenshots, wav) return either inline (with size caps)
or a file path; screenshots are returned as MCP **image content (base64 PNG)** so agents can
actually *see* them, plus a PNG path on disk.

### 5.1 Lifecycle (Tier 1 — first milestone)

| Tool | Params | Backing API | Notes |
|---|---|---|---|
| `n64_load` | `rom` (path), `bios` (path/pak, default `<save>/n64/bios/`), `region` auto/NTSC/PAL, `expansion_pak`, `homebrew_mode`, `deterministic_entropy`, `recompiler`, `gpu` off/on/lavapipe, `save_dir`, `controllers`, `await_debugger` | §3.2 flow | Unloads current game first; returns ROM id, title, region, entry point |
| `n64_unload` | — | `System::unload` | Flushes save paks |
| `n64_power` | `soft_reset` | `root->power(reset)` | Full boot (BIOS runs); soft = CPU reset |
| `n64_status` | — | `system.information`, `cpu.profile`, `rsp.profile`, `rdram.profile`, VI active, screen size, paused, frame count, game-exited flag (XIOCTL), GDB client state | |
| `n64_run` | `frames?`, `ms?` | resume + frame counter | "run 60 frames then auto-pause" is the CI workhorse |
| `n64_pause` | `mode` frame/instruction | frame flag / GDB force-halt | |
| `n64_save_state` / `n64_load_state` | `path?` or `slot`, `inline` | `root->serialize/unserialize` | |

### 5.2 Debugging (Tier 2)

| Tool | Params | Backing API |
|---|---|---|
| `n64_breakpoint` | `set/del/list`, `addr` or `symbol`, `cond?` (addr-based only — no expression eval; see `n64_assert`) | GDB `Z0/Z0,...` via RSP client |
| `n64_watchpoint` | `set/del/list`, `access` r/w/x, `addr`, `size` | GDB `Z2/Z1/Z3` |
| `n64_step` | `count` (default 1), `unit` insn | GDB `s` (recompiler-safe, blocks split at single-step) |
| `n64_next` | — | continue to PC+4 (temp BP) — "step over" |
| `n64_step_to` | `addr` or `symbol` | temp BP + continue |
| `n64_continue` | `timeout_ms` | GDB `c`; returns halt reason: breakpoint/watchpoint/**exception class + PC** (exception mapping in `exceptions.cpp`) |
| `n64_pass_signals` | `signals[]` | GDB `QPassSignals` (already supported) — e.g. ignore TLB-miss storms |
| `n64_read_registers` | `set` gpr/fpu/cop0/hi_lo/pc/rsp | GDB `g` + direct `cpu.ipu.r`, `cpu.fpu.r`, COP0, `rsp.ipu.r`, `rsp.vpu` |
| `n64_write_register` | `reg`, `value` | GDB `G` (recompiler invalidated via `emuCacheInvalidate` hook) |
| `n64_read_memory` | `addr`, `size` (cap 64KiB), `view` virtual/physical/rcp, `as` hex/u8/u16/u32/ascii/cstr, `region` helper (rdram/dcache/ri/mi/vi/ai/pi/si/pif) | `cpu.readDebug*` / `bus.read<..,ARES_DEBUGGER>` |
| `n64_write_memory` | `addr`, `bytes` (hex) or `as` | `cpu.writeDebug*` (paused only) |
| `n64_read_vram` | `which` framebuffer/tmem/tlut/rdp_state, `field`? | **framebuffer**: decode `vi.io.dramAddress` 15/24bpp with VI geometry (reuse `VI::refresh` math) → PNG or hex; **tmem**: `rsp.dmem`/DP TMEM (1KB); **tlut**: RCP TLUT (64KB); **rdp_state**: DPC_*/DPS_* (scissor, combine, fog, env/prim/blend colors, tile/texture/mask images) from `rdp.*` structs |
| `n64_read_rcp_registers` | `which` mi/vi/ai/pi/ri/si/pif/rdram | each peripheral's `io` struct + named debuggers (register names already exist in the `debugger.cpp` files) |
| `n64_screenshot` | `source` scanout/framebuffer, `field`?, `wait_frame`?, `png` path | `Platform::video` grabber (scanout) or raw RDRAM decode (framebuffer); PNG via `nall::Encode::PNG::RGB8`; returned as base64 image + file |
| `n64_audio_record` | `seconds` or `frames`, `path`, `rate` (default 48k) | drain `ai.stream->read()`; `nall::Encode::WAV::stereo<s16>`; can record to next pause |
| `n64_audio_level` | — | stream level / silence flag |
| `n64_input` | `port`, `device` gamepad/rumble/mouse, `buttons[]`, `analog{x,y}` | `controller::gamepad` structs behind the PIF (`virtualPorts` equivalent in core) — inject a frame of input |
| `n64_log_tracer` | `enable/disable/list` for CPU instruction/exception/interrupt/TLB/EMUX, RDP command/IO, RDRAM/PI/SI/MI/VI/AI IO | `Node::Debugger` tree toggles (`->setToggle/enabled`) |
| `n64_log_read` | `filter` component, `limit` | ring buffer from `Platform::log` (includes **emux XLOG/XHEXDUMP** output — game-side prints!) |
| `n64_profile` | — | `cpu.profile` (cycles, exc), `rsp.profile` (cycles, halted), `rdram.profile`, XPROF slots (`cpu.profileSlots`) |
| `n64_gdb_port` | `open/close/status` | `GDB::server.open(port,useIPv4)/close/reset` — let a human attach `gdb-multiarch` in parallel (excludes internal-client tools for that session) |

### 5.3 ROM disassembly helpers (Tier 2/3 — "assist in the ROM disassemblies")

These target the agent workflow: *I have a ROM I'm developing; help me read/patch/verify it.*

| Tool | Purpose | Implementation |
|---|---|---|
| `n64_disassemble` | Disassemble CPU code at a runtime address | `CPU::Disassembler::disassemble(addr, insn)` in a loop; `insn` from `cpu.readDebug<Word>`; live register values in the output; params: `addr`, `count`, `values` (live/static), `follow` (linear vs stop at branches), `max_blocks` |
| `n64_disassemble_range` | Linear block dump over a region (e.g. 0x80246000–0x80246800) | same disassembler; aligned 4-byte words; annotate branch targets; return text (agent-friendly) |
| `n64_disassemble_function` | Given an entry, follow basic blocks (B/J/JAL targets) up to `max_insns`, stop at `jr ra` + `nop` epilogue or loop back-edges | graph walk over the disassembler's decoded branch operands |
| `n64_disassemble_rsp` | RSP microcode (DMEM/SP) disassembly | `RSP::Disassembler::disassemble`; addresses in DMEM (0x04000000+) or via `rsp.dma.current`; include `dpc_status` busy flag so the agent knows if RSP is mid-DMA |
| `n64_read_rom` | Read the *cartridge image file* (not runtime memory): header, ID, entry, region; raw bytes at offset | `mia::Medium` pak file / ROM file via `nall::file`; header decode mirrors `mia/medium/nintendo-64.cpp` |
| `n64_rom_info` | Header + detected compression + game heuristics | detect N64X (0x80371223), N64X2, Compressed-19, Compressed-023, Yosh1 by magic; report header size/entry; (extension: in-place decompress to a working copy — **mia doesn't support these**, so this is genuinely new value for homebrew pipelines) |
| `n64_rom_patch` | Write bytes to the ROM file (in a working copy, keep original) — for iterative dev | file `file_buffer` patch; auto-backup |
| `n64_search_memory` | Find byte/word patterns in RDRAM, a region, or the ROM file | linear scan over `readDebug`/file; e.g. find a texture, a string, a known constant; supports `?` wildcards |
| `n64_symbols` | Load a symbol map (ELF symtab, JSON, or text `addr name`) from the dev toolchain; resolve name↔address for all other tools | new small loader; enables `n64_breakpoint {symbol:"Game_Main"}`, `n64_disassemble_function {symbol:...}` |
| `n64_call` *(experimental)* | Call a function with args: save context, set up $a0–$a3/$v0/$v1/stack, `jr`, wait return, restore, return $v0/$v1 and memory diff | GDB register write + `c` to return-address BP; great for unit-testing homebrew entry points. Flag clearly experimental (caches/TLB state must be flushed: use `cpu.dcache`/`icache` invalidation like the recompiler does) |
| `n64_assert` | Declarative checks: `assert_memory {addr, as, expect, op}`, `assert_register {reg, expect}`, `assert_screen_nonblank`, `assert_audio_playing` — `set/check/clear` | polled on demand or every frame (paused checks are exact; live checks use neutral reads) |
| `n64_wait` | Wait until condition (mem/reg/frame/VI-frame-changed/game-exited) or timeout — "wait for title screen" | frame-loop integration (checked at frame boundary + GDB halt) |
| `n64_frame_diff` *(nice to have)* | Compare current scanout to previous; return changed bounding box / % changed | grabber double-buffer; cheap pixel diff |

### 5.4 Validation workflow (what an agent does with all of this)

1. `n64_load rom=build/n64/homebrew.z64 homebrew_mode=true deterministic_entropy=true gpu=off save_dir=...`
2. `n64_breakpoint {symbol:"Game_Main"}` (after `n64_symbols`), `n64_continue` → halt at main.
3. `n64_disassemble_function` → sanity-check the agent's disassembly against runtime code.
4. `n64_run {frames:1800}` (30 s of game), `n64_screenshot` (agent inspects the image),
   `n64_audio_record {seconds:5, path:...}` (agent can hash/inspect the WAV).
5. `n64_assert {mem: {addr:0x80123456, as:u32, expect:0x1}}`, `n64_read_vram {rdp_state}` to
   check scissor/combine config, `n64_log_read` for XLOG prints.
6. On crash: `n64_status` + last exception (TLB miss at PC, badvaddr from COP0) +
   `n64_disassemble {addr:pc-16, count:8}` → root cause.
7. CI: all of the above in a script, `gpu=off pacing=fast`, exit code from assertions.

---

## 6. Phased implementation plan

**Phase 0 — Headless runner (validate the premise; ~a few hundred lines)**
- CMake: `option(ARES_BUILD_MCP)`; when ON: `ARES_CORES=n64`, skip `ruby/hiro/desktop-ui`
  (+ GUI thirdparty); new `mcp/` subdir, executable `ares-mcp`; keep `deps.json` fetch skippable
  (`ARES_SKIP_DEPS`) since we no longer need ares-deps' SDL/Qt payload — verify sljit/xxhash are
  in-tree (they are: `thirdparty/sljit*`, `ares/n64/..` uses in-tree `xxhash.h`).
- `mcp/platform.cpp` (no-op/log-only), CLI: `ares-mcp run --rom x.z64 --bios ... --frames N
  --gpu off` booting the core on a thread with frame pacing; smoke test: boot a real ROM
  (e.g. any homebrew with XLOG), run N frames, exit code from `Platform::event(Shutdown)`.
- GDB smoke test: same binary + `--gdb-port`; attach `gdb-multiarch`, `break 0x80246xxx`,
  step, `x/10i` — proves the debug path headless before any MCP code.
- Deliverable also doubles as a CI harness.

**Phase 1 — Core host + media capture**
- Full §3 load flow (ports, paks, save dir, options), save states, pacing modes, pause/resume.
- `Platform::video` grabber → `mcp screenshot.png`; audio drain → `mcp audio.wav`
  (CLI flags first, tools later).
- Test: screenshot of a real ROM frame (non-blank assertion), WAV with audible content, save
  state round-trip, controller pak persistence across runs.

**Phase 2 — MCP protocol + Tier 1 tools**
- stdio JSON-RPC 2.0 server (MCP: `initialize`, `tools/list`, `tools/call`, `notifications`),
  vendored single-header JSON lib under `thirdparty/` (e.g. nlohmann/json — MIT, matches the
  vendoring convention).
- Tools: `n64_load/unload/power/status/run/pause/save_state/load_state/screenshot/audio_record/
  read_memory/write_memory`.
- Test with a real MCP client (Claude Desktop config snippet + `mcp` inspector); unit-test each
  tool headless.

**Phase 3 — Tier 2 debug tools**
- In-process GDB RSP client (nall TCP socket); breakpoints/watchpoints/step/next/step_to/
  continue/pass_signals/registers; exception surfacing; `n64_read_vram` (tmem/tlut/rdp_state),
  `n64_read_rcp_registers`, tracers + `n64_log_read` (emux), `n64_profile`, `n64_input`.
- Test: scripted session (break at main, step 10, watchpoint on a save counter, continue to
  exception, dump registers) with assertions on expected values.

**Phase 4 — Disassembly & ROM tools (Tier 3)**
- `n64_disassemble*` (CPU/RSP/function), `n64_read_rom/rom_info/rom_patch`,
  `n64_search_memory`, `n64_symbols`, `n64_assert`, `n64_wait`; N64X/Yosh1/023 detection +
  decompression (new code, self-contained).
- `n64_call` experimental.
- Test: disassemble a known function in a real ROM and diff against expected mnemonics;
  patch-rebuild-run loop on a homebrew.

**Phase 5 — Packaging & docs**
- Static release build (Linux x64 primary; musl static for containers), Dockerfile (no X, no
  GPU, ~200MB), optional `VULKAN=off` build variant that strips the paraLLEl sources for a
  smaller binary (all call sites are already `#if defined(VULKAN)`; only the unconditional
  `target_compile_definitions(ares PUBLIC VULKAN)` in `ares/n64/CMakeLists.txt` and the
  `_parallel_rdp_sources` list need gating).
- `README-mcp.md`: tool reference, MCP client config (stdio command + env), agent workflow
  guide, determinism notes (fixed pacing + `Deterministic Entropy`), GDB co-attach caveats.
- CI: headless boot+frames+screenshot-nonblank+WAV-nonzero+GDB smoke test on every push.

---

## 7. Risks & open questions

1. **GDB server is a single-client singleton.** MCP-internal RSP client vs a human
   `gdb-multiarch` can't coexist. Mitigation: document; optionally add a second listen port /
   read-only mode in a later nall PR.
2. **Frame-boundary pause granularity.** Tools that "read while running" use neutral
   `readDebug` (safe); anything requiring sub-frame consistency must go through the GDB halt
   (which is instruction-precise). Both mechanisms already exist; no core change.
3. **Recompiler + single step.** Upstream already splits blocks at breakpoints and handles the
   PC-override for exceptions; `forceInterpreter` remains the escape hatch (it's a real
   desktop setting, so it's a supported path).
4. **Silent games break audio-driven pacing** → automatic timer fallback (§3.3).
5. **Vulkan code stays compiled in the default build.** Runtime-safe (graceful fallback,
   dynamic loader), but if "zero GPU deps" is a hard requirement, gate the VULKAN define per
   Phase 5. Also: a GPU-less box can opt into **lavapipe** (software Vulkan) for true RDP output
   in CI — exposed as `gpu=lavapipe`.
6. **64DD / Aleck64 / Transfer Pak** — code exists but is out of scope for v1 (load flow
   partially supported via the same `option()`/port machinery; 64DD disk + pif.ntsc.rom are in
   `ares/System/Nintendo 64/`).
7. **`n64_call`** touches TLB/cache state; must invalidate icache/dcache around the call
   (recompiler already has `invalidateSection`); mark experimental and gate behind a flag.
8. **JSON dependency** is the only new third-party addition (single header, MIT); confirm
   acceptable vs writing a minimal JSON-RPC parser (possible but not worth the bug surface).
9. **N64X/Compressed ROMs** aren't loadable by `mia` today (raw header only). Phase 4
   `n64_rom_info`/decompression fills this; alternatively document "provide uncompressed .z64".
10. **Screenshot accuracy with GPU off**: scanout = software VI of the *RDRAM framebuffer*
    (exact for 2D / framebuffer-based output; 3D scenes need GPU on or lavapipe). The tool
    reports which path produced the image so agents can interpret correctly.

---

## 8. Suggested MCP client config (end state)

```json
{
  "mcpServers": {
    "ares-n64": {
      "command": "/opt/ares-mcp/ares-mcp",
      "args": ["mcp", "--gdb-port", "20000", "--save-dir", "/srv/n64-saves"],
      "env": { "N64_BIOS_DIR": "/srv/n64-bios" }
    }
  }
}
```

Agent then has ~30 tools to load, run, break, step, inspect memory/VRAM/disassembly, and capture
screenshots + WAV — all without an X server.
