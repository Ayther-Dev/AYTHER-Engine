#pragma once

// Stable umbrella for the Runtime-facing C++ Engine contract. Public modules
// are added here only after their ownership, lifetime, threading, and error
// rules have been specified and tested.
#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>
#include <ayther/engine/capabilities.hpp>
#include <ayther/engine/core_probe.hpp>
#include <ayther/engine/input.hpp>
#include <ayther/engine/pack.hpp>
#include <ayther/engine/vulkan_interop.hpp>
