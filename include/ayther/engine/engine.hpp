#pragma once

// Stable umbrella for the Runtime-facing C++ Engine contract. Public modules
// are added here only after their ownership, lifetime, threading, and error
// rules have been specified and tested.
#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>
#include <ayther/engine/audio_fact_queue.hpp>
#include <ayther/engine/audio_hd_state.hpp>
#include <ayther/engine/audio_initial_snapshot.hpp>
#include <ayther/engine/audio_observation_close.hpp>
#include <ayther/engine/audio_observation_overflow.hpp>
#include <ayther/engine/audio_observation_worker.hpp>
#include <ayther/engine/audio_observer.hpp>
#include <ayther/engine/audio_pcm_queue.hpp>
#include <ayther/engine/audio_production_limit.hpp>
#include <ayther/engine/capabilities.hpp>
#include <ayther/engine/core_probe.hpp>
#include <ayther/engine/input.hpp>
#include <ayther/engine/pack.hpp>
#include <ayther/engine/vulkan_interop.hpp>
