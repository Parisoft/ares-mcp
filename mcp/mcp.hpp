#pragma once

// Common includes for the ares-mcp headless host.
//
// ares-mcp hosts the N64 core without any GUI, SDL, or display server:
// the core's only frontend-facing interface is ares::Platform
// (see ares/ares/platform.hpp), which we implement in mcp/platform.cpp.

#include <ares/ares.hpp>
#include <n64/n64.hpp>

// library mode: keep <mia/mia.hpp> from pulling in the hiro GUI toolkit
// (and mia's standalone program interface) — we only need the ROM/pak APIs
#define MIA_LIBRARY
#include <mia/mia.hpp>

#include <nall/gdb/server.hpp>
#include <nall/encode/png.hpp>
#include <nall/encode/wav.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace nall;
using namespace nall::primitives;
