#pragma once

#include <mcp/mcp.hpp>

namespace mcp {

// ares::Platform implementation for the headless host.
//
// The ares core only talks to its frontend through this interface. The
// desktop UI provides a "real" one (OpenGL video, SDL audio, hiro dialogs);
// here every callback is forwarded to the CoreHost, which captures frames,
// drains audio, and buffers log output. No display or audio device is
// required: the Screen node's thread composites into memory and calls
// platform->video(), which we hook to grab screenshots.

struct Platform : ares::Platform {
  // Route each node to its vfs::directory (system pak, game pak, save paks).
  std::function<std::shared_ptr<vfs::directory>(ares::Node::Object)> onPak{};

  // Tracer/EMUX messages from core components (channel = "Component Name").
  std::function<void(string channel, string message)> onLog{};

  // Core status messages (e.g. "Vulkan Disabled: No RDP rendering support").
  std::function<void(string message)> onStatus{};

  // Latest composited video frame (ARGB8888, packed).
  std::function<void(const u32* data, u32 width, u32 height)> onVideo{};

  // Core events (homebrew XIOCTL fast-forward / exit, etc.).
  std::function<void(ares::Event)> onEvent{};

  // Controller input state: the core samples every button/axis of each
  // connected controller through input() whenever the game polls (see
  // Gamepad::read()). The desktop UI reports physical controllers here; the
  // MCP host reports the state set by n64_input.
  std::function<void(ares::Node::Input::Input)> onInput{};

  auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
    return onPak ? onPak(node) : nullptr;
  }

  auto log(ares::Node::Debugger::Tracer::Tracer node, string_view message) -> void override {
    if(!onLog) return;
    onLog(string{node->component(), " ", node->name()}, string{message});
  }

  auto status(string_view message) -> void override {
    if(onStatus) onStatus(string{message});
  }

  auto video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void override {
    if(onVideo) onVideo(data, width, height);
  }

  // Audio is consumed directly from the AI stream (see CoreHost::audioLoop);
  // the core only uses this callback to signal that samples are pending.
  auto audio(ares::Node::Audio::Stream) -> void override {}

  auto event(ares::Event event) -> void override {
    if(onEvent) onEvent(event);
  }

  auto input(ares::Node::Input::Input in) -> void override {
    if(onInput) onInput(in);
  }
};

extern Platform platform;

}
