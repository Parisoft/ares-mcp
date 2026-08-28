#pragma once

#include <mcp/mcp.hpp>

namespace mcp {

// Drives the N64 core outside of any GUI.
//
// Replicates the load flow of desktop-ui/emulator/nintendo-64.cpp (ROM +
// system pak + ports + core options) without any UI, then runs emulation on
// a dedicated thread paced to real time. Video frames are captured via the
// Platform::video hook (the Screen node composites into memory even without
// a display), audio is drained from the AI stream, and the built-in GDB
// remote-debug server (nall::GDB::server) is available for breakpoints,
// watchpoints, and single-stepping.

struct CoreHost {
  struct Options {
    string rom{};              // path to the N64 ROM
    string saveDir{"."};       // directory for controller paks / save states
    string region{};           // "" = auto-detect from ROM header
    bool expansionPak = false;
    bool homebrew = false;
    bool deterministic = false;
    bool recompiler = true;
    bool gpu = false;          // Vulkan/paraLLEl RDP rendering (needs a GPU)
    bool gdb = false;
    u32 gdbPort = 20000;
    bool awaitDebugger = false;  // power on, but hold the frame loop until a GDB client connects
  };

  struct LogLine {
    string channel;
    string message;
  };

  // --- lifecycle ---------------------------------------------------------
  auto load(const Options&) -> bool;
  auto power() -> void;
  auto unload() -> void;

  // --- frame loop ----------------------------------------------------------
  auto start() -> void;                 // begin the core thread
  auto stop() -> void;                  // stop the core thread
  auto pause() -> void;
  auto resume() -> void;
  auto paused() const -> bool { return _paused; }
  auto halted() const -> bool { return nall::GDB::server.isHalted(); }
  auto sync() -> void;                  // wait until the in-flight frame (if any) completes
  auto runFrames(u32 frames) -> void;   // block until `frames` more frames run (or stopped)
  auto frames() const -> u64 { return _frameCount; }
  auto videoFrequency() const -> double { return _videoFrequency; }
  auto gameExited() const -> bool { return _gameExited; }

  // --- media ---------------------------------------------------------------
  auto screenshot(string path) -> bool;                  // last captured frame -> PNG
  auto startWavCapture(u32 seconds) -> bool;             // begin capturing `seconds` of audio
  auto finishWavCapture(string path) -> bool;            // wait for capture, encode -> WAV
  auto audioRate() const -> u32;

  // --- log -----------------------------------------------------------------
  auto logRead(u32 limit = 200, string filter = {}) -> std::vector<LogLine>;

  // --- save states -----------------------------------------------------------
  auto saveState(string path) -> bool;
  auto loadState(string path) -> bool;

  // --- info ------------------------------------------------------------------
  auto gameName() const -> string;
  auto region() const -> string { return _region; }
  auto gdbStatus() const -> string;
  auto running() const -> bool { return _running; }
  auto saveDir() const -> const string& { return options.saveDir; }

  // also echo log lines to stderr (CLI debugging aid)
  bool verboseLog = false;

  // core state (exposed for the MCP tool layer)
  ares::Node::System root{};
  std::shared_ptr<mia::Pak> game{};
  std::shared_ptr<mia::Pak> systemPak{};
  std::shared_ptr<mia::Pak> gamepad{};

  // --- controller input --------------------------------------------------------
  // Set button/axis state on a controller port (the core samples it whenever
  // the game polls its controllers).
  //   control: a, b, start, z, l, r, up, down, left, right, cam_up, cam_down,
  //            cam_left, cam_right (buttons) or x, y (analog axes, -100..100)
  //   action:  press (hold until release) | release | tap (auto-release after
  //            `frames` frames)
  // Returns "" on success, otherwise an error message.
  auto setControllerInput(u32 port, const string& control, const string& action,
                          double value, u32 frames) -> string;

private:
  auto coreLoop(uintptr_t) -> void;
  auto audioLoop(uintptr_t) -> void;
  auto appendLog(string channel, string message) -> void;
  auto onVideo(const u32* data, u32 width, u32 height) -> void;
  auto applyInputState(ares::Node::Input::Input in) -> void;  // core thread, per poll
  auto pollTapReleases() -> void;                             // core thread, per frame

  struct InputState {
    bool pressed = false;
    s64 axisValue = 0;
    u64 releaseAtFrame = 0;   // taps: frame at which to auto-release (0 = none)
  };
  std::map<const void*, InputState> _inputState;
  std::mutex _inputMutex;

  Options options{};
  string _region{};
  double _videoFrequency = 60.0;

  atomic<bool> _running{false};
  atomic<bool> _paused{false};
  atomic<bool> _gameExited{false};
  atomic<u64> _frameCount{0};

  nall::thread _coreThread{};
  nall::thread _audioThread{};

  // frame-boundary join
  mutex _frameMutex{};
  condition_variable _frameCond{};
  bool _inFrame = false;

  // video grabber (latest composited frame, ARGB8888 packed)
  mutex _videoMutex{};
  condition_variable _videoCond{};
  std::vector<u32> _videoPixels{};
  u32 _videoWidth = 0;
  u32 _videoHeight = 0;
  bool _videoReady = false;

  // audio capture (pumped from audioLoop)
  mutex _audioMutex{};
  condition_variable _audioCond{};
  bool _capturing = false;
  u64 _captureTarget = 0;
  u64 _captureCount = 0;
  std::vector<s16> _wavLeft{};
  std::vector<s16> _wavRight{};
  u32 _wavRate = 0;

  // log ring
  mutex _logMutex{};
  std::deque<LogLine> _logLines{};
};

}
