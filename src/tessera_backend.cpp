#include "tessera_backend.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

#ifndef AYTHER_HAS_TESSERA
#define AYTHER_HAS_TESSERA 0
#endif

#if AYTHER_HAS_TESSERA
#include <tessera/tessera.h>
#endif

namespace ayther::engine {
namespace {
NativeResult error(NativeStatus status, std::uint32_t phase) noexcept {
  NativeResult result;
  result.code = static_cast<std::int32_t>(status);
  result.phase = phase;
  return result;
}
[[maybe_unused]] NativeResult unavailable(std::uint32_t phase) noexcept {
  return error(NativeStatus::unsupported_capability, phase);
}

#if AYTHER_HAS_TESSERA
template <class T> T initialized() noexcept {
  T value{};
  value.header = {static_cast<std::uint32_t>(sizeof(T)), 1, 0};
  return value;
}
tessera_bytes_v1 bytes(std::span<const std::uint8_t> value) noexcept {
  return {value.data(), static_cast<std::uint64_t>(value.size())};
}
tessera_buffer_v1 buffer(std::span<std::uint8_t> value) noexcept {
  return {value.data(), static_cast<std::uint64_t>(value.size())};
}
NativeFrameId identity(const tessera_frame_id_v1 &value) noexcept {
  return {value.session, value.epoch, value.frame};
}
NativeResult result(tessera_status status,
                    const tessera_context_v1 &context) noexcept {
  return {status,
          context.phase,
          context.os_domain,
          context.os_code,
          identity(context.frame_id),
          context.transaction,
          context.cancel_reason,
          context.flags};
}
NativeResult remember(detail::TesseraBackend &backend, tessera_status status,
                      const tessera_context_v1 &context) noexcept {
  backend.last = result(status, context);
  return backend.last;
}
template <std::size_t N>
bool fixed_string(const char (&source)[N], std::string &out) {
  const auto end = std::find(source, source + N, '\0');
  if (end == source + N)
    return false;
  out.assign(source, end);
  return true;
}
using Exporter = tessera_status(TESSERA_CALL *)(tessera_session,
                                                tessera_buffer_v1,
                                                std::uint64_t *,
                                                tessera_context_v1 *);
NativeResult export_bytes(std::uint64_t handle, std::vector<std::uint8_t> &out,
                          Exporter exporter, std::uint32_t phase) noexcept {
  try {
    auto context = initialized<tessera_context_v1>();
    std::uint64_t required = 0;
    auto status = exporter(handle, {nullptr, 0}, &required, &context);
    if (status != TESSERA_BUFFER_TOO_SMALL && status != TESSERA_OK)
      return result(status, context);
    if (required > 64ULL * 1024 * 1024)
      return error(NativeStatus::budget_exceeded, phase);
    std::vector<std::uint8_t> candidate(static_cast<std::size_t>(required));
    status = exporter(handle, buffer(candidate), &required, &context);
    if (status != TESSERA_OK)
      return result(status, context);
    if (required > candidate.size())
      return error(NativeStatus::backend_failure, phase);
    candidate.resize(static_cast<std::size_t>(required));
    out.swap(candidate);
    return result(status, context);
  } catch (const std::bad_alloc &) {
    return error(NativeStatus::out_of_memory, phase);
  } catch (...) {
    return error(NativeStatus::backend_failure, phase);
  }
}
#endif
} // namespace

namespace detail {
TesseraBackend::~TesseraBackend() {
#if AYTHER_HAS_TESSERA
  if (handle != 0) {
    auto context = initialized<tessera_context_v1>();
    (void)tessera_session_close_v1(handle, &context);
  }
#endif
}
} // namespace detail

NativeSnapshot::~NativeSnapshot() {
  if (handle_ != 0)
    (void)release();
}
NativeSnapshot::NativeSnapshot(NativeSnapshot &&other) noexcept
    : handle_(std::exchange(other.handle_, 0)), last_(other.last_) {}
NativeSnapshot &NativeSnapshot::operator=(NativeSnapshot &&other) noexcept {
  if (this != &other) {
    if (handle_ != 0)
      (void)release();
    handle_ = std::exchange(other.handle_, 0);
    last_ = other.last_;
  }
  return *this;
}
NativeResult NativeSnapshot::last_result() const noexcept { return last_; }
NativeResult NativeSnapshot::retain(NativeSnapshot &out) const noexcept {
  if (&out == this)
    return last_ = error(NativeStatus::invalid_argument, 6);
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_snapshot_retain_v1(handle_, &context);
  last_ = result(status, context);
  if (status == TESSERA_OK) {
    NativeSnapshot candidate;
    candidate.handle_ = handle_;
    candidate.last_ = last_;
    out = std::move(candidate);
  }
#else
  (void)out;
  last_ = unavailable(6);
#endif
  return last_;
}
NativeResult NativeSnapshot::info(NativeSnapshotInfo &out) const noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  auto value = initialized<tessera_snapshot_info_v1>();
  const auto status = tessera_snapshot_query_v1(handle_, &value, &context);
  last_ = result(status, context);
  if (status == TESSERA_OK)
    out = {identity(value.frame_id), value.generation,     value.region_mask,
           value.complete_mask,      value.retained_bytes, value.logical_tick,
           value.region_count};
#else
  (void)out;
  last_ = unavailable(6);
#endif
  return last_;
}
NativeResult NativeSnapshot::region(std::uint32_t index,
                                    NativeRegion &out) const noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  auto value = initialized<tessera_snapshot_region_v1>();
  const auto status =
      tessera_snapshot_region_query_v1(handle_, index, &value, &context);
  last_ = result(status, context);
  if (status == TESSERA_OK)
    out = {value.kind,
           value.encoding,
           value.address,
           value.byte_count,
           value.element_bytes,
           value.element_count,
           {value.data.data, static_cast<std::size_t>(value.data.bytes)},
           value.bank,
           value.flags};
#else
  (void)index;
  (void)out;
  last_ = unavailable(6);
#endif
  return last_;
}
NativeResult NativeSnapshot::release() noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_snapshot_release_v1(handle_, &context);
  last_ = result(status, context);
  if (status == TESSERA_OK)
    handle_ = 0;
#else
  last_ = unavailable(6);
#endif
  return last_;
}

NativeEmulation::NativeEmulation()
    : backend_(std::make_unique<detail::TesseraBackend>()) {}
NativeEmulation::~NativeEmulation() = default;
NativeResult NativeEmulation::last_result() const noexcept {
  return backend_->last;
}

NativeResult
NativeEmulation::create(const NativeConfig &configuration,
                        std::unique_ptr<NativeEmulation> &out) noexcept {
#if AYTHER_HAS_TESSERA
  try {
    auto context = initialized<tessera_context_v1>();
    auto api = initialized<tessera_api_info_v1>();
    auto status = tessera_api_query_v1(1, 0, &api, &context);
    if (status != TESSERA_OK)
      return result(status, context);
    if (api.major != 1 || api.pointer_bytes != sizeof(void *) ||
        api.max_alignment > 8)
      return error(NativeStatus::abi_incompatible, 1);
    std::unique_ptr<NativeEmulation> candidate(new NativeEmulation());
    auto creation = initialized<tessera_session_create_info_v1>();
    creation.backend_id = {
        reinterpret_cast<const std::uint8_t *>(configuration.backend_id.data()),
        static_cast<std::uint64_t>(configuration.backend_id.size())};
    creation.content = bytes(configuration.content);
    creation.config = bytes(configuration.config);
    creation.initial_persistent_memory =
        bytes(configuration.initial_persistent_memory);
    creation.system_override = configuration.system_override;
    status = tessera_session_create_v1(&creation, &candidate->backend_->handle,
                                       &context);
    const auto created = remember(*candidate->backend_, status, context);
    if (status == TESSERA_OK)
      out = std::move(candidate);
    return created;
  } catch (const std::bad_alloc &) {
    return error(NativeStatus::out_of_memory, 3);
  } catch (...) {
    return error(NativeStatus::backend_failure, 3);
  }
#else
  (void)configuration;
  (void)out;
  return unavailable(3);
#endif
}

NativeResult
NativeEmulation::backends(std::vector<NativeBackendInfo> &out) noexcept {
#if AYTHER_HAS_TESSERA
  try {
    auto context = initialized<tessera_context_v1>();
    std::uint32_t required = 0;
    auto status =
        tessera_backends_enumerate_v1(nullptr, 0, &required, &context);
    if (status != TESSERA_BUFFER_TOO_SMALL && status != TESSERA_OK)
      return result(status, context);
    if (required > 1024)
      return error(NativeStatus::budget_exceeded, 6);
    std::vector<tessera_backend_info_v1> values(required);
    for (auto &value : values)
      value = initialized<tessera_backend_info_v1>();
    const auto capacity = required;
    status = tessera_backends_enumerate_v1(values.data(), capacity, &required,
                                           &context);
    if (status != TESSERA_OK)
      return result(status, context);
    if (required > capacity)
      return error(NativeStatus::backend_failure, 6);
    std::vector<NativeBackendInfo> candidate;
    candidate.reserve(required);
    for (std::size_t index = 0; index < required; ++index) {
      const auto &value = values[index];
      NativeBackendInfo item;
      item.kind = value.kind;
      item.system = value.system;
      item.capabilities = value.capabilities;
      if (!fixed_string(value.id, item.id) ||
          !fixed_string(value.version, item.version))
        return error(NativeStatus::backend_failure, 6);
      std::copy(std::begin(value.build_id), std::end(value.build_id),
                item.build_id.begin());
      candidate.push_back(std::move(item));
    }
    out.swap(candidate);
    return result(status, context);
  } catch (const std::bad_alloc &) {
    return error(NativeStatus::out_of_memory, 6);
  } catch (...) {
    return error(NativeStatus::backend_failure, 6);
  }
#else
  (void)out;
  return unavailable(6);
#endif
}

NativeResult NativeEmulation::detect(std::span<const std::uint8_t> content,
                                     std::uint32_t system_override,
                                     NativeDetection &out) noexcept {
#if AYTHER_HAS_TESSERA
  try {
    auto context = initialized<tessera_context_v1>();
    auto value = initialized<tessera_detection_v1>();
    std::uint32_t required = 0;
    auto status = tessera_detect_v1(bytes(content), system_override, &value,
                                    nullptr, 0, &required, &context);
    const auto semantic = [](tessera_status code) {
      return code == TESSERA_OK || code == TESSERA_UNKNOWN_CONTENT ||
             code == TESSERA_AMBIGUOUS_CONTENT ||
             code == TESSERA_INVALID_CONTENT;
    };
    if (status != TESSERA_BUFFER_TOO_SMALL && !semantic(status))
      return result(status, context);
    if (required > 1024)
      return error(NativeStatus::budget_exceeded, 2);
    std::vector<tessera_candidate_v1> values(required);
    const auto capacity = required;
    status = tessera_detect_v1(bytes(content), system_override, &value,
                               values.data(), capacity, &required, &context);
    if (!semantic(status))
      return result(status, context);
    if (required > capacity)
      return error(NativeStatus::backend_failure, 2);
    NativeDetection candidate;
    candidate.outcome = value.outcome;
    candidate.system = value.system;
    candidate.evidence = value.evidence;
    candidate.evidence_offset = value.evidence_offset;
    candidate.evidence_bytes = value.evidence_bytes;
    std::copy(std::begin(value.content_sha256), std::end(value.content_sha256),
              candidate.content_sha256.begin());
    candidate.candidates.reserve(required);
    for (std::size_t index = 0; index < required; ++index) {
      const auto &item = values[index];
      candidate.candidates.push_back(
          {item.system, item.evidence_class, item.evidence});
    }
    out = std::move(candidate);
    return result(status, context);
  } catch (const std::bad_alloc &) {
    return error(NativeStatus::out_of_memory, 2);
  } catch (...) {
    return error(NativeStatus::backend_failure, 2);
  }
#else
  (void)content;
  (void)system_override;
  (void)out;
  return unavailable(2);
#endif
}

NativeResult NativeEmulation::info(NativeSessionInfo &out) noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  auto value = initialized<tessera_session_info_v1>();
  const auto status =
      tessera_session_query_v1(backend_->handle, &value, &context);
  const auto queried = remember(*backend_, status, context);
  if (status == TESSERA_OK) {
    NativeSessionInfo candidate;
    candidate.frame_id = identity(value.frame_id);
    candidate.generation = value.generation;
    candidate.logical_tick = value.logical_tick;
    candidate.tick_hz_num = value.tick_hz_num;
    candidate.tick_hz_den = value.tick_hz_den;
    candidate.capabilities = value.capabilities;
    candidate.state = value.state;
    candidate.system = value.system;
    std::copy(std::begin(value.content_sha256), std::end(value.content_sha256),
              candidate.content_sha256.begin());
    candidate.pending_transaction = value.pending_transaction;
    candidate.next_replay_frame = value.next_replay_frame;
    std::copy(std::begin(value.backend_build_id),
              std::end(value.backend_build_id),
              candidate.backend_build_id.begin());
    out = candidate;
  }
  return queried;
#else
  (void)out;
  return backend_->last = unavailable(6);
#endif
}

NativeResult NativeEmulation::step(const NativeInputFrame &input) noexcept {
#if AYTHER_HAS_TESSERA
  if (input.assignments.size() > 64 ||
      (!input.assignments.empty() && input.assignments.data() == nullptr))
    return backend_->last = error(NativeStatus::invalid_argument, 1);
  std::array<tessera_input_assignment_v1, 64> assignments{};
  for (std::size_t index = 0; index < input.assignments.size(); ++index) {
    const auto &value = input.assignments[index];
    assignments[index] = {value.port, value.control, value.value};
  }
  auto frame = initialized<tessera_input_frame_v1>();
  frame.epoch = input.epoch;
  frame.frame = input.frame;
  frame.assignments = assignments.data();
  frame.count = static_cast<std::uint32_t>(input.assignments.size());
  auto context = initialized<tessera_context_v1>();
  const auto status =
      tessera_session_step_v1(backend_->handle, &frame, &context);
  return remember(*backend_, status, context);
#else
  (void)input;
  return backend_->last = unavailable(4);
#endif
}

NativeResult NativeEmulation::pause() noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_session_pause_v1(backend_->handle, &context);
  return remember(*backend_, status, context);
#else
  return backend_->last = unavailable(1);
#endif
}
NativeResult NativeEmulation::reset() noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_session_reset_v1(backend_->handle, &context);
  return remember(*backend_, status, context);
#else
  return backend_->last = unavailable(5);
#endif
}
NativeResult NativeEmulation::close() noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_session_close_v1(backend_->handle, &context);
  if (status == TESSERA_OK)
    backend_->handle = 0;
  return remember(*backend_, status, context);
#else
  return backend_->last = unavailable(10);
#endif
}
NativeResult
NativeEmulation::bind_trace(std::span<const std::uint8_t> trace) noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status =
      tessera_session_bind_trace_v1(backend_->handle, bytes(trace), &context);
  return remember(*backend_, status, context);
#else
  (void)trace;
  return backend_->last = unavailable(1);
#endif
}
NativeResult NativeEmulation::replay_next() noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status =
      tessera_session_replay_next_v1(backend_->handle, &context);
  return remember(*backend_, status, context);
#else
  return backend_->last = unavailable(4);
#endif
}
NativeResult NativeEmulation::submit_controls(
    const NativeControlTransaction &controls) noexcept {
#if AYTHER_HAS_TESSERA
  if (controls.items.size() > 64 ||
      (!controls.items.empty() && controls.items.data() == nullptr))
    return backend_->last = error(NativeStatus::invalid_argument, 1);
  std::array<tessera_control_item_v1, 64> items{};
  for (std::size_t index = 0; index < controls.items.size(); ++index) {
    const auto &item = controls.items[index];
    items[index] = {item.id, item.value_type, item.value};
  }
  auto value = initialized<tessera_control_transaction_v1>();
  value.epoch = controls.epoch;
  value.frame = controls.frame;
  value.id = controls.id;
  value.items = items.data();
  value.count = static_cast<std::uint32_t>(controls.items.size());
  auto context = initialized<tessera_context_v1>();
  const auto status =
      tessera_controls_submit_v1(backend_->handle, &value, &context);
  return remember(*backend_, status, context);
#else
  (void)controls;
  return backend_->last = unavailable(1);
#endif
}

NativeResult NativeEmulation::original(NativeFrame &out) noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  auto value = initialized<tessera_original_frame_v1>();
  const auto status =
      tessera_original_frame_query_v1(backend_->handle, &value, &context);
  const auto queried = remember(*backend_, status, context);
  if (status == TESSERA_OK)
    out = {identity(value.frame_id),
           value.width,
           value.height,
           value.row_bytes,
           value.video_format,
           value.aspect_num,
           value.aspect_den,
           {value.pixels.data, static_cast<std::size_t>(value.pixels.bytes)},
           {value.pcm.data, static_cast<std::size_t>(value.pcm.bytes)},
           value.sample_rate,
           value.channels,
           value.pcm_frames,
           value.tick_begin,
           value.tick_end};
  return queried;
#else
  (void)out;
  return backend_->last = unavailable(6);
#endif
}
NativeResult NativeEmulation::state(std::vector<std::uint8_t> &out) noexcept {
#if AYTHER_HAS_TESSERA
  return backend_->last = export_bytes(backend_->handle, out,
                                       &tessera_state_export_canonical_v1, 6);
#else
  (void)out;
  return backend_->last = unavailable(6);
#endif
}
NativeResult NativeEmulation::events(std::uint64_t cursor,
                                     std::span<std::uint8_t> out,
                                     std::uint64_t &required,
                                     std::uint64_t &next) noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_events_read_v1(
      backend_->handle, cursor, buffer(out), &required, &next, &context);
  return remember(*backend_, status, context);
#else
  (void)cursor;
  (void)out;
  (void)required;
  (void)next;
  return backend_->last = unavailable(6);
#endif
}
NativeResult NativeEmulation::losses(std::span<std::uint8_t> out,
                                     std::uint64_t &required) noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_losses_query_v1(backend_->handle, buffer(out),
                                              &required, &context);
  return remember(*backend_, status, context);
#else
  (void)out;
  (void)required;
  return backend_->last = unavailable(6);
#endif
}
NativeResult NativeEmulation::snapshot_acquire(NativeSnapshot &out) noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  NativeSnapshot candidate;
  const auto status = tessera_snapshot_acquire_v1(backend_->handle,
                                                  &candidate.handle_, &context);
  candidate.last_ = remember(*backend_, status, context);
  if (status == TESSERA_OK)
    out = std::move(candidate);
  return backend_->last;
#else
  (void)out;
  return backend_->last = unavailable(6);
#endif
}
NativeResult
NativeEmulation::checkpoint(std::vector<std::uint8_t> &out) noexcept {
#if AYTHER_HAS_TESSERA
  return backend_->last = export_bytes(backend_->handle, out,
                                       &tessera_checkpoint_export_v1, 7);
#else
  (void)out;
  return backend_->last = unavailable(7);
#endif
}
NativeResult NativeEmulation::restore_checkpoint(
    std::span<const std::uint8_t> checkpoint,
    std::span<const std::uint8_t> trace) noexcept {
#if AYTHER_HAS_TESSERA
  auto context = initialized<tessera_context_v1>();
  const auto status = tessera_checkpoint_restore_v1(
      backend_->handle, bytes(checkpoint), bytes(trace), &context);
  return remember(*backend_, status, context);
#else
  (void)checkpoint;
  (void)trace;
  return backend_->last = unavailable(9);
#endif
}

} // namespace ayther::engine
