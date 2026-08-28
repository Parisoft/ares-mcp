<img src="https://github.com/ares-emulator/ares/blob/master/ares/ares/resource/logo@2x.png" width="350"/>

[![License: ISC](https://img.shields.io/badge/License-ISC-blue.svg)](https://github.com/higan-emu/ares/blob/master/LICENSE)

# ares-mcp

This repository is a fork of
[ares](https://github.com/ares-emulator/ares) — a multi-system emulator that
began development on October 14th, 2004, as a descendant of
[higan](https://github.com/higan-emu/higan) and
[bsnes](https://github.com/bsnes/bsnes/), focused on accuracy and preservation.

This fork adds a **headless Nintendo 64 core host** that speaks the
[Model Context Protocol (MCP)](https://modelcontextprotocol.io): an
`ares-mcp` server that loads and drives N64 ROMs over JSON-RPC 2.0 on stdio,
with no GUI, no SDL, and no X server. AI agents (or any MCP client) can load a
ROM, advance emulation frame by frame, send controller input, capture
screenshots and audio, read the emulator log, and attach a GDB client —
useful for validating and debugging N64 games in CI-style environments with no
display.

## What this fork adds

* __`ares-mcp`__ (`mcp/`) — the headless N64 host, in two modes:
  * `ares-mcp mcp` — MCP server (JSON-RPC 2.0 over stdio) with ten `n64_*` tools
  * `ares-mcp run` — plain headless CLI runner (ROM in; frames, screenshot,
    WAV, save states, GDB port out)
* __Build support__ — `cmake -DARES_BUILD_MCP=ON` builds only the N64 core and
  the MCP server, skipping the GUI toolkit (hiro), the SDL output layer
  (ruby), the desktop application, and prebuilt binary dependencies.
* __N64 core fixes required for headless use__:
  * CP0 `COUNT` reads (`mfc0`/`dmfc0 $9`) and the timer interrupt now account
    for clocks accumulated since the last synchronization — games that use the
    free-running counter as their clock (e.g. [FlappyBird-N64](https://github.com/meeq/FlappyBird-N64))
    see correct real-time ticks
  * RDRAM hidden-bit shadow memory is allocated and null-guarded on the
    software path
  * All diagnostics are routed to stderr so stdout carries only the MCP
    protocol stream

Everything else is stock ares; the upstream GUI remains fully buildable.

## Building

Requirements: CMake ≥ 3.28, a C++20 compiler (GCC 12+ / Clang 15+), Ninja.

```sh
cmake -B build -G Ninja -DARES_BUILD_MCP=ON
ninja -C build mcp
```

The server binary is produced at `build/rundir/bin/ares-mcp`.

Notes:

* `-DARES_BUILD_MCP=ON` restricts the build to the N64 core plus `mcp/` —
  nothing GUI-related is compiled, so no SDL/GTK/Qt or display is needed.
* Omit the option to build upstream ares (all cores + GUI) as usual.
* Video: the N64 RDP is rendered through ares' paraLLEl (Vulkan) backend. On a
  machine without a Vulkan GPU the RDP produces no output — emulation, input,
  audio, and state still work, and CPU-blitted content is visible, but
  RDP-drawn graphics (which is how most N64 games render) will not appear.
  Pass `--gpu` (MCP: `n64_load` `gpu: true`) where a Vulkan driver exists.

## Running

### As an MCP server

```sh
ares-mcp mcp [--verbose]
```

The server speaks JSON-RPC 2.0 on **stdout**; all log/diagnostic output goes
to **stderr** and never corrupts the protocol stream. Point any MCP client at
it:

```json
{
  "mcpServers": {
    "ares-n64": {
      "command": "/path/to/ares-mcp",
      "args": ["mcp"]
    }
  }
}
```

Typical agent workflow: `n64_load` a ROM (the console powers on), `n64_run` to
advance N frames, `n64_screenshot` to see the screen, `n64_input` to send
controller input, `n64_log`/`n64_status` to inspect what happened.

### Headless CLI

```
Usage:
  ares-mcp mcp [options]     Run as an MCP (Model Context Protocol) server
  ares-mcp run [options]     Run a ROM headless (CLI)

Options:
  --rom <path>              N64 ROM to load (.z64/.n64/.v64)
  --save-dir <dir>          Directory for save files (default: current directory)
  --region <auto|ntsc|pal>  Region override (default: from the ROM header)
  --expansion               Emulate the 8MB Expansion Pak
  --homebrew                Enable homebrew mode (emux extensions)
  --deterministic           Deterministic entropy (reproducible runs)
  --interpreter             Force the CPU/RSP interpreter (no recompiler)
  --gpu                     Enable Vulkan/paraLLEl RDP rendering (needs a GPU)
  --gdb-port <port>         Open the GDB remote debug server on 127.0.0.1:<port>
  --await-debugger          Hold execution until a GDB client connects
  --frames <n>              Stop after n frames (default: until interrupted)
  --screenshot <path>       Save the final frame as a PNG
  --wav <path>              Record audio to a WAV file
  --wav-seconds <n>         Audio capture length in seconds (default: whole run)
  --state-in <path>         Load a save state before running
  --state-out <path>        Save a state when the run ends
  --verbose                 Echo core log messages to stderr
```

Example:

```sh
ares-mcp run --rom FlappyBird.z64 --frames 1200 --screenshot frame.png --wav audio.wav
```

## Tools

| Tool | Parameters | What it does |
|---|---|---|
| `n64_status` | — | Console state: ROM loaded (name, region), running/paused, frame count, GDB status |
| `n64_load` | `rom`, `save_dir?`, `region?`, `expansion_pak?`, `homebrew?`, `recompiler?`, `deterministic?`, `gpu?` | Load a ROM and power on the console (replaces any loaded game) |
| `n64_run` | `frames?` (default 60, max 36000), `stop?` (default true) | Advance emulation N frames (1 frame = 1 VI refresh, ~16.7 ms NTSC); blocks until done |
| `n64_input` | `port?` (1-4), `control`, `action?` (`press`/`release`/`tap`), `value?` (axes, -100..100), `frames?` (tap hold, default 6) | Send controller input (see below) |
| `n64_screenshot` | `path?` | Capture the current video output as PNG (returned inline + saved to disk) |
| `n64_log` | `limit?` (default 100, max 1000), `filter?` | Read the emulator log: core messages, boot progress, exceptions |
| `n64_record` | `seconds?` (default 5, max 300), `path?` | Record audio to a WAV file (core must be running — `n64_run` with `stop: false`) |
| `n64_pause` | — | Frame-accurate pause |
| `n64_resume` | — | Resume a paused core |
| `n64_stop` | — | Stop the core; the ROM stays loaded and `n64_run` resumes from the current frame |

## Controller input (`n64_input`)

All 16 buttons and the analog stick, on any of the four controller ports:

| Type | Controls |
|---|---|
| Buttons | `a`, `b`, `start`, `z`, `l`, `r`, `up`, `down`, `left`, `right`, `cam_up`, `cam_down`, `cam_left`, `cam_right` |
| Analog stick | `x`, `y` (value -100..100; `release` recenters) |

Actions:

| Action | Behavior |
|---|---|
| `tap` (default) | Press, then auto-release after `frames` frames (default 6) — the usual way to flick a button |
| `press` | Hold until an explicit `release` |
| `release` | Unpress (or recenter an axis) |

Input is applied on the core thread at the moment the game polls its
controller, so a press is never lost across a frame boundary.

Example — start a game from the title screen:

```json
{ "control": "start", "action": "tap" }
```

## Example session

```
n64_load     { "rom": "FlappyBird.z64" }
n64_run      { "frames": 240 }                  # boot to the title screen
n64_screenshot {}                                # see the logo
n64_input    { "control": "start", "action": "tap" }
n64_run      { "frames": 30 }                    # -> "Get Ready"
n64_input    { "control": "a", "action": "tap" } # flap
n64_run      { "frames": 60 }
n64_screenshot {}
n64_log      { "filter": "exception" }
n64_status   {}
```

## Testing

```sh
python3 mcp/test/mcp_client.py
```

Runs the end-to-end suite against `build/rundir/bin/ares-mcp` using the
generated test ROM (`mcp/test/green.z64` — regenerate with
`python3 mcp/test/make_green_rom.py mcp/test/ipl3_seed.bin mcp/test/green.z64`):
protocol handshake, tool catalog, load/run, screenshot content checks
(non-black frame, expected pixels), controller input round-trips and error
handling, log and status behavior, and clean shutdown.

Test ROMs are not committed to this repository (see `.gitignore` for
provenance).

## Debugging

The N64 core includes a complete GDB Remote Serial Protocol server (software
breakpoints, read/write/access watchpoints, single step, registers, memory
access). In CLI mode:

```sh
ares-mcp run --rom game.z64 --gdb-port 20000 --await-debugger
```

then `target remote 127.0.0.1:20000` from GDB. `n64_status` reports the GDB
server state in MCP mode.

## Project layout

```
mcp/            the headless N64 host (MCP server + CLI)
  server.cpp    MCP protocol server (tools, JSON-RPC 2.0 over stdio)
  core-host.cpp core lifecycle: power/run/stop, input, video, audio, states
  platform.hpp  ares Platform interface implementation (headless)
  test/         e2e test client + test-ROM generators
ares/n64/       the Nintendo 64 core (upstream ares, plus the fixes above)
ares/ares/      core framework
nall/           Near's alternative to the C++ standard library (GDB server, TCP, ...)
libco/          cooperative multithreading library
mia/            ROM database and loader
```

## Upstream

This fork tracks
[ares-emulator/ares](https://github.com/ares-emulator/ares). Official ares
releases are on [the ares website](https://ares-emu.net); upstream build
guides for the full multi-system build live in the
[ares wiki](https://github.com/ares-emulator/ares/wiki). Questions and chat:
[ares Discord](https://discord.com/invite/gz2quhk2kv).
