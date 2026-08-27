#pragma once

#include <mcp/json.hpp>
#include <mcp/core-host.hpp>

#include <vector>

namespace mcp {

// MCP (Model Context Protocol) server: newline-delimited JSON-RPC 2.0 over
// stdin/stdout. Exposes the headless N64 core as MCP tools so a language
// model can load ROMs, run frames, capture video/audio, and read logs.
//
// stdout carries protocol messages ONLY; every diagnostic goes to stderr.
//
// Protocol: MCP 2024-11-05 / 2025-03-26 (tools only, no prompts/resources).

struct McpServer {
  explicit McpServer(CoreHost& host) : _host(host) {}

  auto run() -> int;                    // main loop; returns 0 on EOF/clean exit
  auto setVerbose(bool value) -> void { _verbose = value; }

private:
  struct Tool {
    string name;
    string description;
    Json inputSchema;
    auto (*handler)(McpServer& self, const Json& args) -> Json;
  };

  // --- protocol layer -----------------------------------------------------
  auto dispatch(const Json& request) -> maybe<Json>;     // null => notification
  auto handleInitialize(const Json* params) -> Json;
  auto handleToolsList() -> Json;

  static auto makeResult(const Json& id, const Json& result) -> Json;
  static auto makeError(const Json& id, i32 code, const string& message) -> Json;

  static auto resultText(const string& text) -> Json;    // tools/call result, single text item
  static auto toolError(const string& message) -> Json;  // tools/call result, isError:true

  // --- tools ----------------------------------------------------------------
  static auto toolStatus(McpServer& self, const Json& args) -> Json;
  static auto toolLoad(McpServer& self, const Json& args) -> Json;
  static auto toolRun(McpServer& self, const Json& args) -> Json;
  static auto toolScreenshot(McpServer& self, const Json& args) -> Json;
  static auto toolLog(McpServer& self, const Json& args) -> Json;
  static auto toolRecord(McpServer& self, const Json& args) -> Json;
  static auto toolPause(McpServer& self, const Json& args) -> Json;
  static auto toolResume(McpServer& self, const Json& args) -> Json;
  static auto toolStop(McpServer& self, const Json& args) -> Json;

  auto tools() const -> const std::vector<Tool>&;

  // --- plumbing --------------------------------------------------------------
  auto log(const string& message) -> void;               // stderr, always
  auto send(const Json& response) -> void;               // stdout, one line, flushed

  CoreHost& _host;
  bool _verbose = false;
};

}
