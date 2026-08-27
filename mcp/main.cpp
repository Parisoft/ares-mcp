#include <mcp/core-host.hpp>
#include <mcp/platform.hpp>
#include <mcp/server.hpp>

#include <nall/arguments.hpp>

#include <csignal>
#include <cstring>
#include <execinfo.h>

namespace mcp {
Platform platform;
}

namespace {

volatile sig_atomic_t g_interrupted = 0;

auto onSignal(int) -> void {
  g_interrupted = 1;
}

auto utoa(u64 value) -> string {
  return string{value};
}

auto usage() -> void {
  print(
    "ares-mcp — headless N64 core host (MCP server)\n",
    "\n",
    "Usage:\n",
    "  ares-mcp mcp [options]     Run as an MCP (Model Context Protocol) server\n",
    "  ares-mcp run [options]     Run a ROM headless (CLI)\n",
    "\n",
    "MCP options:\n",
    "  --verbose                  Echo protocol traffic to stderr\n",
    "\n",
    "Options:\n",
    "  --rom <path>              N64 ROM to load (.z64/.n64/.v64)\n",
    "  --save-dir <dir>          Directory for save files (default: current directory)\n",
    "  --region <auto|ntsc|pal>  Region override (default: from the ROM header)\n",
    "  --expansion               Emulate the 8MB Expansion Pak\n",
    "  --homebrew                Enable homebrew mode (emux extensions)\n",
    "  --deterministic           Deterministic entropy (reproducible runs)\n",
    "  --interpreter             Force the CPU/RSP interpreter (no recompiler)\n",
    "  --gpu                     Enable Vulkan/paraLLEl RDP rendering (needs a GPU)\n",
    "  --gdb-port <port>         Open the GDB remote debug server on 127.0.0.1:<port>\n",
    "  --await-debugger          Hold execution until a GDB client connects\n",
    "  --frames <n>              Stop after n frames (default: until interrupted)\n",
    "  --screenshot <path>       Save the final frame as a PNG\n",
    "  --wav <path>              Record audio to a WAV file\n",
    "  --wav-seconds <n>         Audio capture length in seconds (default: whole run)\n",
    "  --state-in <path>         Load a save state before running\n",
    "  --state-out <path>        Save a state when the run ends\n",
    "  --verbose                 Echo core log messages to stderr\n"
  );
}

auto mcpServer(int argc, char** argv) -> int {
  mcp::CoreHost host;
  mcp::McpServer server(host);
  for(int i = 0; i < argc; i++) {
    if(std::strcmp(argv[i], "--verbose") == 0) server.setVerbose(true);
  }

  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  int result = server.run();

  host.unload();
  return result;
}

auto run(nall::Arguments args) -> int {
  auto options = mcp::CoreHost::Options{};
  string value;

  if(!args.take("--rom", options.rom)) {
    print("ares-mcp: --rom <path> is required");
    return 1;
  }
  args.take("--save-dir", options.saveDir);
  if(!options.saveDir) options.saveDir = ".";
  args.take("--region", options.region);
  if(options.region == "auto") options.region = {};
  options.expansionPak = args.take("--expansion");
  options.homebrew = args.take("--homebrew");
  options.deterministic = args.take("--deterministic");
  options.recompiler = !args.take("--interpreter");
  options.gpu = args.take("--gpu");
  if(args.take("--gdb-port", value)) {
    options.gdb = true;
    options.gdbPort = (u32)value.integer();
  }
  options.awaitDebugger = args.take("--await-debugger");
  u64 frames = 0;
  if(args.take("--frames", value)) frames = (u64)value.integer();
  string screenshot, wav, stateIn, stateOut;
  args.take("--screenshot", screenshot);
  args.take("--wav", wav);
  u32 wavSeconds = 0;
  if(args.take("--wav-seconds", value)) wavSeconds = (u32)value.integer();
  args.take("--state-in", stateIn);
  args.take("--state-out", stateOut);

  mcp::CoreHost host;
  host.verboseLog = args.take("--verbose");

  if(!host.load(options)) return 1;

  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  host.power();
  print("game:  ", host.gameName());
  print("region: ", host.region(), "  expansion: ", options.expansionPak ? "on" : "off",
        "  homebrew: ", options.homebrew ? "on" : "off",
        "  gpu: ", options.gpu ? "on" : "off",
        "  recompiler: ", options.recompiler ? "on" : "off");
  print(host.gdbStatus());

  if(stateIn) {
    if(!host.loadState(stateIn)) {
      print("ares-mcp: failed to load state: ", stateIn);
      return 1;
    }
    print("state: loaded from ", stateIn);
  }

  host.start();

  if(wav) {
    u32 seconds = wavSeconds;
    if(!seconds) seconds = (u32)(frames ? (frames + 59) / 60 + 5 : 60);
    host.startWavCapture(seconds);
  }

  if(frames) {
    host.runFrames((u32)frames);
  } else {
    while(!g_interrupted && !host.gameExited()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  host.stop();

  if(wav) {
    if(host.finishWavCapture(wav)) print("audio: ", wav);
    else print("ares-mcp: no audio captured");
  }
  if(screenshot) {
    if(host.screenshot(screenshot)) print("screenshot: ", screenshot);
    else print("ares-mcp: no video frame captured");
  }
  if(stateOut) {
    if(host.saveState(stateOut)) print("state: saved to ", stateOut);
    else print("ares-mcp: failed to save state");
  }

  print("frames: ", utoa(host.frames()));
  if(host.gameExited()) print("game exited via XIOCTL");

  host.unload();
  return 0;
}

}

auto crashHandler(int sig) -> void {
  void* frames[64];
  int n = backtrace(frames, 64);
  fprintf(stderr, "CRASH: signal %d\n", sig);
  backtrace_symbols_fd(frames, n, 2);
  _exit(139);
}

auto main(int argc, char** argv) -> int {
  //temporary: capture crash backtraces (remove before commit)
  signal(SIGSEGV, crashHandler);
  signal(SIGBUS, crashHandler);
  signal(SIGFPE, crashHandler);

  //the core talks to the frontend exclusively through this pointer
  ares::platform = &mcp::platform;

  if(argc < 2) {
    usage();
    return 1;
  }

  //the subcommand is parsed from the raw argv: nall::Arguments rewrites any
  //argument that matches a directory in the working directory to an absolute
  //path, which would mangle the "mcp" subcommand when run from the repo root
  auto command = string{argv[1]};

  if(command == "mcp") {
    return mcpServer(argc - 2, argv + 2);
  }
  if(command == "help" || command == "--help" || command == "-h") {
    usage();
    return 0;
  }
  if(command == "run") {
    std::vector<string> options;
    for(int i = 2; i < argc; i++) options.push_back(argv[i]);
    return run(nall::Arguments{options});
  }

  print("ares-mcp: unknown command: ", command, "\n");
  usage();
  return 1;
}
