#pragma once

// Stable umbrella for the Runtime-facing Engine contract. Public modules are
// added here only after their ownership, lifetime, threading, and error rules
// have been specified and tested.
#include <ayther/engine/capabilities.hpp>
#include <ayther/engine/core_probe.hpp>
#include <ayther/engine/pack.hpp>
#include <ayther/engine/vulkan_interop.hpp>
