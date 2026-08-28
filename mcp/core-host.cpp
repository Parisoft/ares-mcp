#include <mcp/core-host.hpp>
#include <mcp/platform.hpp>

#include <nall/gdb/server.hpp>

#include <chrono>
#include <thread>

using namespace ares::Nintendo64;
using namespace std::chrono;

namespace mcp {

// --- lifecycle -------------------------------------------------------------

auto CoreHost::load(const Options& opts) -> bool {
  options = opts;

  //wire the platform callbacks (once; they point at this host)
  if(!platform.onPak) {
    platform.onPak = [this](ares::Node::Object node) -> std::shared_ptr<vfs::directory> {
      if(!node) return nullptr;
      auto name = node->name();
      if(name == "Nintendo 64") return systemPak ? systemPak->pak : nullptr;
      if(name == "Nintendo 64 Cartridge") return game ? game->pak : nullptr;
      if(name == "Gamepad") return gamepad ? gamepad->pak : nullptr;
      return nullptr;
    };
    platform.onLog = [this](string channel, string message) -> void {
      //The ISViewer tracer (libdragon emulog/debugf) is a terminal tracer with
      //autoLineBreak off, so it notifies one character at a time. Reassemble
      //those into lines (split on '\n') so n64_log shows readable output.
      if(channel == "Cartridge ISViewer") {
        appendISViewerLine(message);
        return;
      }
      //Any other log activity means the ISViewer stream is done for now;
      //flush any partial line so it is not lost.
      flushISViewerLine();
      appendLog(channel, message);
    };
    platform.onStatus = [this](string message) -> void {
      appendLog("STATUS", message);
    };
    platform.onVideo = [this](const u32* data, u32 width, u32 height) -> void {
      onVideo(data, width, height);
    };
    platform.onEvent = [this](ares::Event event) -> void {
      if(event == ares::Event::Shutdown) {
        _gameExited = true;
        appendLog("STATUS", "game requested exit (XIOCTL)");
      }
    };
    platform.onInput = [this](ares::Node::Input::Input in) -> void {
      applyInputState(in);
    };
  }

  //clean up any previous session
  stop();
  if(root) {
    root->unload();
    root = nullptr;
  }

  //1) game: parse the ROM header, build the in-memory game pak
  game = mia::Medium::create("Nintendo 64");
  if(auto result = game->load(options.rom); result != successful) {
    print("ares-mcp: failed to load ROM: ", options.rom);
    if(result.info) print("ares-mcp: ", result.info);
    return false;
  }
  appendLog("STATUS", string{"ROM loaded: "
    , game->pak->attribute("title"), " ["
    , game->pak->attribute("region"), ", CIC "
    , game->pak->attribute("cic"), "]"});

  //2) system: PIF ROMs come from built-in resources; no external BIOS needed
  systemPak = mia::System::create("Nintendo 64");
  if(systemPak->load() != successful) {
    print("ares-mcp: failed to load system pak");
    return false;
  }

  //3) region (from the ROM header, or forced)
  _region = options.region;
  if(!_region) _region = game->pak->attribute("region");
  bool pal = (_region == "PAL");
  _videoFrequency = pal ? 50.0 : 60.0;

  //4) core options (see option() in ares/n64/system/system.cpp)
  ares::Nintendo64::option("Enable GPU acceleration", options.gpu);
  ares::Nintendo64::option("Homebrew Mode", options.homebrew);
  ares::Nintendo64::option("Deterministic Entropy", options.deterministic);
  ares::Nintendo64::option("Recompiler", options.recompiler);
  ares::Nintendo64::option("Expansion Pak", options.expansionPak);

  //5) core (the GDB server must be reset BEFORE the core loads: the core's
  //    System::initDebugHooks() wires the debugger hooks, which reset() clears)
  if(options.gdb) GDB::server.reset();
  auto name = string{"[Nintendo] Nintendo 64 (", pal ? "PAL" : "NTSC", ")"};
  if(!ares::Nintendo64::load(root, name)) {
    print("ares-mcp: failed to load the N64 core");
    return false;
  }

  //6) cartridge
  if(auto port = root->find<ares::Node::Port>("Cartridge Slot")) {
    port->allocate();
    port->connect();
  }

  //7) controller 1 (+ controller pak persisted in saveDir)
  if(auto port = root->find<ares::Node::Port>("Controller Port 1")) {
    auto peripheral = port->allocate("Gamepad");
    port->connect();
    if(auto pakPort = peripheral->find<ares::Node::Port>("Pak")) {
      bool needsCpak = game->pak->attribute({"port", 1, "/cpak"}).boolean() ||
                       game->pak->attribute("cic") == "6105";  //64MB carts use a controller pak
      if(needsCpak) {
        directory::create(options.saveDir);
        gamepad = mia::Pak::create("Nintendo 64");
        gamepad->pak->append("save.pak", 32_KiB);
        gamepad->load("save.pak", ".pak", options.saveDir);
        pakPort->allocate("Controller Pak");
        pakPort->connect();
      }
    }
  }

  //8) GDB remote debug server (the core's debugger hooks are wired in via
  //   System::initDebugHooks() during the core load above; reset() was run
  //   BEFORE that load, and this just opens the TCP listener)
  if(options.gdb) {
    GDB::server.close();  //stop the listener from a previous session
    if(!GDB::server.open(options.gdbPort, true)) {
      print("ares-mcp: failed to open the GDB server on port ", (string)options.gdbPort);
      return false;
    }
  }

  return true;
}

auto CoreHost::power() -> void {
  if(!root) return;
  _frameCount = 0;
  _gameExited = false;
  {
    lock_guard<mutex> lock(_inputMutex);
    _inputState.clear();  //fresh power-on: all inputs neutral
  }
  root->power();
}

auto CoreHost::unload() -> void {
  stop();

  if(root) {
    root->unload();  //the core saves its paks (in-memory) before unloading
    root = nullptr;
  }

  //persist save files to saveDir
  if(game) game->save(options.saveDir);
  if(gamepad) gamepad->save("save.pak", ".pak", options.saveDir);

  GDB::server.close();
  game.reset();
  systemPak.reset();
  gamepad.reset();
  _region = {};
  _frameCount = 0;
  _gameExited = false;
  {
    lock_guard<mutex> lock(_inputMutex);
    _inputState.clear();
  }
}

// --- frame loop --------------------------------------------------------------

auto CoreHost::start() -> void {
  if(_running) return;
  _running = true;
  _coreThread = nall::thread::create(std::bind_front(&CoreHost::coreLoop, this));
  _audioThread = nall::thread::create(std::bind_front(&CoreHost::audioLoop, this));
}

auto CoreHost::stop() -> void {
  if(!_running) return;
  _running = false;
  _coreThread.join();
  _audioThread.join();
  _frameCond.notify_all();
  _audioCond.notify_all();
  flushISViewerLine();  //capture any trailing text that had no final newline
}

auto CoreHost::pause() -> void {
  _paused = true;
}

auto CoreHost::resume() -> void {
  _paused = false;
}

auto CoreHost::sync() -> void {
  unique_lock<mutex> lock(_frameMutex);
  _frameCond.wait(lock, [&] { return !_inFrame || !_running; });
}

auto CoreHost::runFrames(u32 frames) -> void {
  auto target = _frameCount + frames;
  unique_lock<mutex> lock(_frameMutex);
  _frameCond.wait(lock, [&] {
    return _frameCount >= target || _gameExited || !_running;
  });
}

auto CoreHost::coreLoop(uintptr_t) -> void {
  nall::thread::setName("dev.ares.mcp.core");
  auto last = steady_clock::now();

  while(_running) {
    if(!root) {
      std::this_thread::sleep_for(milliseconds(10));
      continue;
    }

    //a debugger (breakpoint / single-step / exception) halts execution;
    //service the GDB server until it resumes
    if(GDB::server.isHalted()) {
      GDB::server.updateLoop();
      continue;
    }

    if(_paused || (options.awaitDebugger && options.gdb && !GDB::server.hasClient())) {
      GDB::server.updateLoop();
      std::this_thread::sleep_for(milliseconds(5));
      continue;
    }

    GDB::server.updateLoop();

    {
      lock_guard<mutex> lock(_frameMutex);
      _inFrame = true;
    }
    root->run();  //one frame of simulation; returns at VI refresh
    _frameCount++;
    pollTapReleases();
    {
      lock_guard<mutex> lock(_frameMutex);
      _inFrame = false;
      _frameCond.notify_all();
    }

    GDB::server.updateLoop();

    //pace to real time (NTSC 60Hz / PAL 50Hz); if the CPU cannot keep up,
    //we simply fall behind rather than spinning (no fast-forward requested)
    auto period = nanoseconds((u64)(1'000'000'000.0 / _videoFrequency));
    auto now = steady_clock::now();
    if(auto elapsed = now - last; elapsed < period) {
      std::this_thread::sleep_for(period - elapsed);
    }
    last = steady_clock::now();
  }
}

auto CoreHost::audioLoop(uintptr_t) -> void {
  nall::thread::setName("dev.ares.mcp.audio");

  while(_running) {
    if(!root || !ai.stream) {
      std::this_thread::sleep_for(milliseconds(5));
      continue;
    }

    //drain the resampler so its buffer never grows unbounded; the desktop
    //UI consumes this via SDL, we discard it unless a capture is active
    if(ai.stream->pending()) {
      f64 sample[2] = {0.0, 0.0};
      auto channels = ai.stream->read(sample);
      if(_capturing) {
        auto toS16 = [](f64 value) -> s16 {
          if(value > 1.0) value = 1.0;
          if(value < -1.0) value = -1.0;
          return (s16)(value * 32767.0);
        };
        lock_guard<mutex> lock(_audioMutex);
        _wavLeft.push_back(toS16(sample[0]));
        if(channels > 1) _wavRight.push_back(toS16(sample[1]));
        _captureCount++;
        if(_captureCount >= _captureTarget) {
          _capturing = false;
          _audioCond.notify_all();
        }
      }
    } else {
      std::this_thread::sleep_for(milliseconds(1));
    }
  }
}

// --- media ---------------------------------------------------------------------

auto CoreHost::onVideo(const u32* data, u32 width, u32 height) -> void {
  {
    lock_guard<mutex> lock(_videoMutex);
    _videoPixels.assign(data, data + width * height);
    _videoWidth = width;
    _videoHeight = height;
    _videoReady = true;
  }
  _videoCond.notify_all();
}

auto CoreHost::screenshot(string path) -> bool {
  //The Screen node composites asynchronously on its own thread (it outlives
  //stop(), which only joins the core/audio loops); wait for the first frame.
  {
    unique_lock<mutex> lock(_videoMutex);
    _videoCond.wait_for(lock, std::chrono::seconds(30), [&] { return _videoReady; });
  }
  std::vector<u32> pixels;
  u32 width = 0, height = 0;
  {
    lock_guard<mutex> lock(_videoMutex);
    if(!_videoReady) return false;
    pixels = _videoPixels;
    width = _videoWidth;
    height = _videoHeight;
  }
  return Encode::PNG::RGB8(path, pixels.data(), width * sizeof(u32), width, height);
}

// --- controller input --------------------------------------------------------

auto CoreHost::setControllerInput(u32 port, const string& control, const string& action,
                                  double value, u32 frames) -> string {
  if(port < 1 || port > 4) return "port must be 1-4";
  if(action != "press" && action != "release" && action != "tap")
    return "action must be 'press', 'release', or 'tap'";

  static const ControllerPort* ports[] = {
    nullptr, &controllerPort1, &controllerPort2, &controllerPort3, &controllerPort4
  };
  auto* gamepad = dynamic_cast<ares::Nintendo64::Gamepad*>(ports[port]->device.get());
  if(!gamepad) return string{"no gamepad attached to port ", port};

  ares::Node::Input::Input node;
  if(control == "a") node = gamepad->a;
  else if(control == "b") node = gamepad->b;
  else if(control == "start") node = gamepad->start;
  else if(control == "z") node = gamepad->z;
  else if(control == "l") node = gamepad->l;
  else if(control == "r") node = gamepad->r;
  else if(control == "up") node = gamepad->up;
  else if(control == "down") node = gamepad->down;
  else if(control == "left") node = gamepad->left;
  else if(control == "right") node = gamepad->right;
  else if(control == "cam_up") node = gamepad->cameraUp;
  else if(control == "cam_down") node = gamepad->cameraDown;
  else if(control == "cam_left") node = gamepad->cameraLeft;
  else if(control == "cam_right") node = gamepad->cameraRight;
  else if(control == "x") node = gamepad->x;
  else if(control == "y") node = gamepad->y;
  else return string{"unknown control '", control,
    "' (use a, b, start, z, l, r, up, down, left, right, cam_up, cam_down, "
    "cam_left, cam_right, x, y)"};
  if(!node) return string{"no gamepad attached to port ", port};

  const bool isAxis = (control == "x" || control == "y");
  if(isAxis) {
    if(action == "release") value = 0.0;  //release centers the stick
    if(value > 100.0) value = 100.0;
    if(value < -100.0) value = -100.0;
  }

  {
    lock_guard<mutex> lock(_inputMutex);
    auto& s = _inputState[static_cast<const void*>(node.get())];
    if(isAxis) {
      s.axisValue = (s64)(value * 32767.0 / 100.0);
      s.releaseAtFrame = 0;
    } else if(action == "release") {
      s.pressed = false;
      s.releaseAtFrame = 0;
    } else {  //press or tap
      s.pressed = true;
      s.releaseAtFrame = (action == "tap") ? (_frameCount + frames) : 0;
    }
  }
  return {};
}

//called from the core thread every time the game polls its controllers
// (the platform callback contract: set the current value of each node)
auto CoreHost::applyInputState(ares::Node::Input::Input in) -> void {
  if(auto button = std::dynamic_pointer_cast<ares::Core::Input::Button>(in)) {
    bool value = false;
    {
      lock_guard<mutex> lock(_inputMutex);
      if(auto it = _inputState.find(static_cast<const void*>(in.get())); it != _inputState.end())
        value = it->second.pressed;
    }
    button->setValue(value);
  } else if(auto axis = std::dynamic_pointer_cast<ares::Core::Input::Axis>(in)) {
    s64 value = 0;
    {
      lock_guard<mutex> lock(_inputMutex);
      if(auto it = _inputState.find(static_cast<const void*>(in.get())); it != _inputState.end())
        value = it->second.axisValue;
    }
    axis->setValue(value);
  }
}

//core thread, once per frame: auto-release taps that are done
auto CoreHost::pollTapReleases() -> void {
  lock_guard<mutex> lock(_inputMutex);
  for(auto& [node, state] : _inputState) {
    if(state.releaseAtFrame && _frameCount >= state.releaseAtFrame) {
      state.pressed = false;
      state.releaseAtFrame = 0;
    }
  }
}

auto CoreHost::audioRate() const -> u32 {
  return root && ai.stream ? (u32)ai.stream->frequency() : 0;
}

auto CoreHost::startWavCapture(u32 seconds) -> bool {
  if(!root || !ai.stream) return false;
  _wavRate = (u32)ai.stream->frequency();
  {
    lock_guard<mutex> lock(_audioMutex);
    if(_capturing) return false;
    _captureTarget = (u64)seconds * _wavRate;
    _captureCount = 0;
    _wavLeft.clear();
    _wavRight.clear();
    _capturing = true;
  }
  return true;
}

auto CoreHost::finishWavCapture(string path) -> bool {
  {
    unique_lock<mutex> lock(_audioMutex);
    //bound the wait: a game that produces no audio never fills the capture,
    //and we must not hold the caller (e.g. the MCP server) hostage forever
    u64 seconds = _wavRate ? _captureTarget / _wavRate : 30;
    if(seconds < 1) seconds = 1;
    _audioCond.wait_for(lock, (seconds * 2 + 30) * 1s, [&] { return !_capturing || !_running; });
  }
  if(_wavLeft.empty()) return false;
  if(ai.stream && ai.stream->channels() > 1) {
    return Encode::WAV::stereo<s16>(
      path,
      std::span<const s16>{_wavLeft.data(), _wavLeft.size()},
      std::span<const s16>{_wavRight.data(), _wavRight.size()},
      _wavRate
    );
  }
  return Encode::WAV::mono<s16>(
    path,
    std::span<const s16>{_wavLeft.data(), _wavLeft.size()},
    _wavRate
  );
}

// --- log -------------------------------------------------------------------------

auto CoreHost::appendLog(string channel, string message) -> void {
  lock_guard<mutex> lock(_logMutex);
  _logLines.push_back(LogLine{channel, message});
  if(_logLines.size() > 4000) _logLines.pop_front();
}

auto CoreHost::appendISViewerLine(string message) -> void {
  lock_guard<mutex> lock(_logMutex);
  _pendingISViewer.append(message);
  while(true) {
    auto pos = _pendingISViewer.find("\n");
    if(!pos) break;
    auto line = _pendingISViewer.slice(0, pos.get());
    _pendingISViewer.remove(0, pos.get() + 1);
    _logLines.push_back(LogLine{"Cartridge ISViewer", line});
    if(_logLines.size() > 4000) _logLines.pop_front();
  }
}

auto CoreHost::flushISViewerLine() -> void {
  lock_guard<mutex> lock(_logMutex);
  if(_pendingISViewer.size() == 0) return;
  _logLines.push_back(LogLine{"Cartridge ISViewer", _pendingISViewer});
  _pendingISViewer.reset();
  if(_logLines.size() > 4000) _logLines.pop_front();
}

auto CoreHost::logRead(u32 limit, string filter) -> std::vector<LogLine> {
  std::vector<LogLine> result;
  lock_guard<mutex> lock(_logMutex);
  for(auto it = _logLines.rbegin(); it != _logLines.rend() && result.size() < limit; ++it) {
    if(filter && !it->channel.find(filter) && !it->message.find(filter)) continue;
    result.push_back(*it);
  }
  std::reverse(result.begin(), result.end());
  return result;
}

// --- save states --------------------------------------------------------------------

auto CoreHost::saveState(string path) -> bool {
  if(!root) return false;
  sync();
  auto state = root->serialize(true);
  if(!state) return false;
  return file::write(path, std::span<const u8>{state.data(), state.size()});
}

auto CoreHost::loadState(string path) -> bool {
  if(!root) return false;
  auto data = file::read(path);
  if(data.empty()) return false;
  sync();
  serializer state(data.data(), (u32)data.size());
  return root->unserialize(state);
}

// --- info -----------------------------------------------------------------------------

auto CoreHost::gameName() const -> string {
  return root ? root->game() : string{};
}

auto CoreHost::gdbStatus() const -> string {
  if(!options.gdb) return "GDB server disabled";
  return GDB::server.getStatusText(options.gdbPort, true);
}

}
