#include <mcp/server.hpp>

#include <nall/encode/base64.hpp>
#include <nall/file.hpp>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace mcp {

namespace {

// --- input-schema helpers (JSON Schema, the subset MCP uses) ----------------

auto prop(const string& name, const string& type, const string& description) -> Json {
  Json p = Json::makeObject();
  p.set("type", Json::makeString(type));
  p.set("description", Json::makeString(description));
  return p;
}

auto propEnum(const string& name, const string& description, const std::vector<string>& values) -> Json {
  Json p = prop(name, "string", description);
  Json items = Json::makeArray();
  for(auto& v : values) items.items.push_back(Json::makeString(v));
  p.set("enum", items);
  return p;
}

auto schema(const Json& properties, const std::vector<string>& required = {}) -> Json {
  Json s = Json::makeObject();
  s.set("type", Json::makeString("object"));
  s.set("properties", properties);
  if(!required.empty()) {
    Json req = Json::makeArray();
    for(auto& r : required) req.items.push_back(Json::makeString(r));
    s.set("required", req);
  }
  return s;
}

}

// --- main loop ----------------------------------------------------------------

auto McpServer::run() -> int {
  log("MCP server started (JSON-RPC 2.0 over stdio); awaiting requests");

  std::string line;
  while(std::getline(std::cin, line)) {
    if(!line.empty() && line.back() == '\r') line.pop_back();
    if(line.empty()) continue;

    if(_verbose) log(string{"recv: ", line.data()});

    auto request = Json::parse(line.data());
    if(!request) {
      send(makeError(Json::makeNull(), -32700, "parse error"));
      continue;
    }

    if(auto response = dispatch(*request)) {
      if(_verbose) log(string{"send: ", response->print()});
      send(*response);
    }
  }

  log("EOF on stdin; shutting down");
  return 0;
}

auto McpServer::send(const Json& response) -> void {
  auto line = response.print();
  std::fputs(line.data(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

auto McpServer::log(const string& message) -> void {
  std::fprintf(stderr, "[ares-mcp] %s\n", message.data());
  std::fflush(stderr);
}

// --- protocol layer -------------------------------------------------------------

auto McpServer::makeResult(const Json& id, const Json& result) -> Json {
  Json response = Json::makeObject();
  response.set("jsonrpc", Json::makeString("2.0"));
  response.set("id", id);
  response.set("result", result);
  return response;
}

auto McpServer::makeError(const Json& id, i32 code, const string& message) -> Json {
  Json error = Json::makeObject();
  error.set("code", Json::makeNumber((double)code));
  error.set("message", Json::makeString(message));
  Json response = Json::makeObject();
  response.set("jsonrpc", Json::makeString("2.0"));
  response.set("id", id);
  response.set("error", error);
  return response;
}

auto McpServer::dispatch(const Json& request) -> maybe<Json> {
  const auto* idJson = request.find("id");
  const bool isNotification = !idJson;
  Json id = isNotification ? Json::makeNull() : *idJson;
  auto method = request.getString("method");
  const auto* params = request.find("params");

  //notifications never get a response (JSON-RPC 2.0)
  if(isNotification) {
    if(_verbose) log(string{"notification: ", method});
    return nothing;
  }

  if(!method) {
    return makeError(id, -32600, "invalid request: missing 'method'");
  }

  if(method == "initialize") {
    return makeResult(id, handleInitialize(params));
  }

  if(method == "ping") {
    return makeResult(id, Json::makeObject());
  }

  if(method == "tools/list") {
    return makeResult(id, handleToolsList());
  }

  if(method == "tools/call") {
    auto name = params ? params->getString("name") : "";
    const Tool* tool = nullptr;
    if(name) {
      for(const auto& t : tools()) {
        if(t.name == name) { tool = &t; break; }
      }
    }
    if(!tool) {
      return makeError(id, -32602, name ? string{"unknown tool: ", name} : "invalid params: missing tool 'name'");
    }

    const auto* argsJson = params ? params->find("arguments") : nullptr;
    Json args = argsJson ? *argsJson : Json::makeObject();

    Json result;
    try {
      result = tool->handler(*this, args);
    } catch(const std::exception& e) {
      result = toolError(e.what());
    } catch(...) {
      result = toolError("internal error in tool handler");
    }
    return makeResult(id, result);
  }

  if(_verbose) log(string{"unknown method: ", method});
  return makeError(id, -32601, string{"method not found: ", method});
}

auto McpServer::handleInitialize(const Json* params) -> Json {
  //echo the client's protocol version if we support it, else advertise ours
  string version = "2025-03-26";
  if(params) {
    auto clientVersion = params->getString("protocolVersion");
    if(clientVersion == "2024-11-05" || clientVersion == "2025-03-26" || clientVersion == "2025-06-18") {
      version = clientVersion;
    }
  }

  Json capabilities = Json::makeObject();
  capabilities.set("tools", Json::makeObject());

  Json serverInfo = Json::makeObject();
  serverInfo.set("name", Json::makeString("ares-mcp"));
  serverInfo.set("version", Json::makeString("1.0.0"));

  Json result = Json::makeObject();
  result.set("protocolVersion", Json::makeString(version));
  result.set("capabilities", capabilities);
  result.set("serverInfo", serverInfo);
  result.set("instructions", Json::makeString(
    "Headless Nintendo 64 emulator. Workflow: n64_load a ROM (the console powers on and "
    "runs), then n64_run to advance N frames (it stops by default), n64_screenshot to see "
    "the screen, n64_log to read core messages, n64_record to capture audio. Use n64_pause/"
    "n64_resume/n64_stop to control the running core."));
  return result;
}

auto McpServer::handleToolsList() -> Json {
  Json list = Json::makeArray();
  for(const auto& t : tools()) {
    Json tool = Json::makeObject();
    tool.set("name", Json::makeString(t.name));
    tool.set("description", Json::makeString(t.description));
    tool.set("inputSchema", t.inputSchema);
    list.items.push_back(tool);
  }
  Json result = Json::makeObject();
  result.set("tools", list);
  return result;
}

// --- tools/call result helpers ---------------------------------------------------

auto McpServer::resultText(const string& text) -> Json {
  Json item = Json::makeObject();
  item.set("type", Json::makeString("text"));
  item.set("text", Json::makeString(text));
  Json content = Json::makeArray();
  content.items.push_back(item);
  Json result = Json::makeObject();
  result.set("content", content);
  return result;
}

auto McpServer::toolError(const string& message) -> Json {
  auto result = resultText(string{"ERROR: ", message});
  result.set("isError", Json::makeBool(true));
  return result;
}

// --- tools --------------------------------------------------------------------------

auto McpServer::tools() const -> const std::vector<Tool>& {
  static const std::vector<Tool> table = {
    Tool{
      "n64_status",
      "Query the console state: whether a ROM is loaded (name, region), whether the core is "
      "running or paused, the frame count, and GDB status.",
      schema(Json::makeObject()),
      &McpServer::toolStatus
    },
    Tool{
      "n64_load",
      "Load an N64 ROM and power on the console (replaces any loaded game). The core starts "
      "running immediately; use n64_run to advance a controlled number of frames.",
      schema(
        (Json::makeObject())
          .set("rom", prop("rom", "string", "Path to the N64 ROM file (.z64/.n64/.v64)."))
          .set("save_dir", prop("save_dir", "string", "Directory for controller paks and save states (default: the server's working directory)."))
          .set("region", propEnum("region", "Force the region instead of auto-detecting from the ROM header.", {"ntsc", "pal"}))
          .set("expansion_pak", prop("expansion_pak", "boolean", "Emulate the 8MB Expansion Pak (default: false)."))
          .set("homebrew", prop("homebrew", "boolean", "Enable homebrew mode / emux extensions (default: false)."))
          .set("recompiler", prop("recompiler", "boolean", "Use the recompiler (default: true); false forces the interpreter."))
          .set("deterministic", prop("deterministic", "boolean", "Deterministic entropy for reproducible runs (default: false)."))
          .set("gpu", prop("gpu", "boolean", "Enable GPU-accelerated RDP (default: false; requires a GPU, not available headless).")),
        {"rom"}),
      &McpServer::toolLoad
    },
    Tool{
      "n64_run",
      "Advance emulation by N frames (one frame = one VI refresh, ~16.7ms NTSC). Blocks until "
      "the frames are done. By default the core stops afterwards (idle, no CPU); pass stop=false "
      "to leave the console running.",
      schema(
        (Json::makeObject())
          .set("frames", prop("frames", "integer", "Number of frames to run (default: 60, max 36000 per call)."))
          .set("stop", prop("stop", "boolean", "Stop the core when the frames are done (default: true)."))),
      &McpServer::toolRun
    },
    Tool{
      "n64_screenshot",
      "Capture the current video output as a PNG image (returns the image inline and saves it "
      "to a file).",
      schema(
        (Json::makeObject())
          .set("path", prop("path", "string", "Output PNG path (default: <save_dir>/ares-screenshot.png)."))),
      &McpServer::toolScreenshot
    },
    Tool{
      "n64_log",
      "Read the emulator log: core messages, boot progress, exceptions, and status lines.",
      schema(
        (Json::makeObject())
          .set("limit", prop("limit", "integer", "Max number of lines from the tail (default: 100, max 1000)."))
          .set("filter", prop("filter", "string", "Only lines containing this substring (case-sensitive)."))),
      &McpServer::toolLog
    },
    Tool{
      "n64_record",
      "Record N seconds of audio from the running core and encode it to a WAV file. The core "
      "must be running (use n64_run with stop=false).",
      schema(
        (Json::makeObject())
          .set("seconds", prop("seconds", "number", "Capture length in seconds (default: 5, max 300)."))
          .set("path", prop("path", "string", "Output WAV path (default: <save_dir>/ares-record.wav)."))),
      &McpServer::toolRecord
    },
    Tool{
      "n64_pause",
      "Pause a running core (frame-accurate; the console holds its current state).",
      schema(Json::makeObject()),
      &McpServer::toolPause
    },
    Tool{
      "n64_resume",
      "Resume a paused core.",
      schema(Json::makeObject()),
      &McpServer::toolResume
    },
    Tool{
      "n64_stop",
      "Stop the core. The ROM stays loaded; n64_run resumes from the current frame.",
      schema(Json::makeObject()),
      &McpServer::toolStop
    },
  };
  return table;
}

auto McpServer::toolStatus(McpServer& self, const Json&) -> Json {
  const bool loaded = self._host.game != nullptr;

  Json s = Json::makeObject();
  s.set("loaded", Json::makeBool(loaded));
  if(loaded) {
    s.set("name", Json::makeString(self._host.gameName()));
    s.set("region", Json::makeString(self._host.region()));
  }
  s.set("running", Json::makeBool(self._host.running()));
  s.set("paused", Json::makeBool(self._host.paused()));
  s.set("frames", Json::makeNumber((double)self._host.frames()));
  s.set("video_hz", Json::makeNumber(self._host.videoFrequency()));
  s.set("gdb", Json::makeString(self._host.gdbStatus()));
  s.set("game_exited", Json::makeBool(self._host.gameExited()));
  return resultText(s.print());
}

auto McpServer::toolLoad(McpServer& self, const Json& args) -> Json {
  auto rom = args.getString("rom");
  if(!rom) return toolError("'rom' (path to the N64 ROM) is required");
  if(!file::exists(rom)) return toolError(string{"ROM not found: ", rom});

  CoreHost::Options o;
  o.rom = rom;
  o.saveDir = args.getString("save_dir", self._host.saveDir());
  if(!o.saveDir || o.saveDir == ".") o.saveDir = ".";
  auto region = args.getString("region");
  if(region == "ntsc" || region == "pal") o.region = region;
  o.expansionPak = args.getBool("expansion_pak", false);
  o.homebrew = args.getBool("homebrew", false);
  o.recompiler = args.getBool("recompiler", true);
  o.deterministic = args.getBool("deterministic", false);
  o.gpu = args.getBool("gpu", false);

  //load() cleans up any previous session (stop + unload) on its own
  if(!self._host.load(o)) return toolError(string{"failed to load ROM: ", rom});
  self._host.power();
  self._host.start();

  string msg = string{"Loaded '", self._host.gameName(), "' [", self._host.region(),
    "]. The console is on and running — use n64_run to advance N frames, n64_screenshot to "
    "capture video, n64_stop to halt."};
  return resultText(msg);
}

auto McpServer::toolRun(McpServer& self, const Json& args) -> Json {
  if(self._host.game == nullptr) return toolError("no ROM loaded — call n64_load first");

  auto framesD = args.getNumber("frames", 60);
  u32 frames = (u32)framesD;
  if(frames < 1) frames = 1;
  if(frames > 36000) return toolError("'frames' too large (max 36000 per call)");
  const bool stop = args.getBool("stop", true);

  if(!self._host.running()) self._host.start();
  self._host.runFrames(frames);
  if(stop) self._host.stop();

  string msg = string{"Ran ", frames, " frames (total ", self._host.frames(), ")"};
  if(self._host.gameExited()) msg.append(". The game exited via XIOCTL.");
  else if(stop) msg.append(". Core stopped — n64_run resumes from the current frame.");
  else msg.append(". Core still running — n64_pause or n64_stop to halt it.");
  return resultText(msg);
}

auto McpServer::toolScreenshot(McpServer& self, const Json& args) -> Json {
  if(self._host.game == nullptr) return toolError("no ROM loaded — call n64_load first");

  auto path = args.getString("path");
  if(!path) {
    path = self._host.saveDir();
    if(!path || path == ".") path = "./ares-screenshot.png";
    else path.append("/ares-screenshot.png");
  }

  if(!self._host.screenshot(path)) {
    return toolError("no video frame captured — run some frames first (n64_run)");
  }

  auto bytes = file::read(path);
  if(bytes.size() == 0) return toolError(string{"failed to read the encoded PNG: ", path});

  Json image = Json::makeObject();
  image.set("type", Json::makeString("image"));
  image.set("data", Json::makeString(Encode::Base64(bytes.data(), (u32)bytes.size(), "MIME")));
  image.set("mimeType", Json::makeString("image/png"));

  Json text = Json::makeObject();
  text.set("type", Json::makeString("text"));
  text.set("text", Json::makeString(string{"PNG saved to ", path}));

  Json content = Json::makeArray();
  content.items.push_back(image);
  content.items.push_back(text);

  Json result = Json::makeObject();
  result.set("content", content);
  return result;
}

auto McpServer::toolLog(McpServer& self, const Json& args) -> Json {
  auto limitD = args.getNumber("limit", 100);
  u32 limit = (u32)limitD;
  if(limit < 1) limit = 100;
  if(limit > 1000) limit = 1000;
  auto filter = args.getString("filter");

  auto lines = self._host.logRead(limit, filter);
  if(lines.empty()) return resultText("(log is empty)");

  string out;
  for(const auto& line : lines) {
    out.append(line.channel).append(": ").append(line.message).append('\n');
  }
  return resultText(out);
}

auto McpServer::toolRecord(McpServer& self, const Json& args) -> Json {
  if(self._host.game == nullptr) return toolError("no ROM loaded — call n64_load first");
  if(!self._host.running()) return toolError("the core is not running — call n64_run with stop=false first");

  auto secondsD = args.getNumber("seconds", 5);
  u32 seconds = (u32)secondsD;
  if(seconds < 1) seconds = 1;
  if(seconds > 300) return toolError("'seconds' too large (max 300)");

  auto path = args.getString("path");
  if(!path) {
    path = self._host.saveDir();
    if(!path || path == ".") path = "./ares-record.wav";
    else path.append("/ares-record.wav");
  }

  if(!self._host.startWavCapture(seconds)) return toolError("failed to start audio capture");
  if(!self._host.finishWavCapture(path)) return toolError("no audio captured (is the game producing sound?)");

  return resultText(string{"Recorded ", seconds, " seconds of audio to ", path});
}

auto McpServer::toolPause(McpServer& self, const Json&) -> Json {
  if(!self._host.running()) return toolError("the core is not running");
  self._host.pause();
  return resultText("Paused.");
}

auto McpServer::toolResume(McpServer& self, const Json&) -> Json {
  if(self._host.game == nullptr) return toolError("no ROM loaded — call n64_load first");
  self._host.resume();
  return resultText("Resumed.");
}

auto McpServer::toolStop(McpServer& self, const Json&) -> Json {
  if(self._host.game == nullptr) return toolError("no ROM loaded — call n64_load first");
  self._host.stop();
  return resultText("Stopped. The ROM stays loaded — n64_run resumes from the current frame.");
}

}
