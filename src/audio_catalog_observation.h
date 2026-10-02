#pragma once

#include "audio_observation_ids.h"
#include <ayther/ayther_core_ffi.h>

#include <array>
#include <string_view>

namespace ayther::audio_qa {

// Preparation-only adapter for the actual Core parser. All callback storage is
// fixed. At most 4096 distinct signatures are tracked for duplicate detection;
// further entries are still emitted, but unknown duplicate status is explicit.
class CatalogRecorder final {
public:
  CatalogRecorder(observation::Observer observer, IdentitySource &identities,
                  std::string_view source) noexcept
      : observer_(observer), identities_(identities), source_(source) {}
  CatalogRecorder(const CatalogRecorder &) = delete;
  CatalogRecorder &operator=(const CatalogRecorder &) = delete;
  CatalogRecorder(CatalogRecorder &&) = delete;
  CatalogRecorder &operator=(CatalogRecorder &&) = delete;
  ~CatalogRecorder() = default;

  // Failure before parsing must remain distinguishable from an empty array.
  // This path needs no recorder allocation, including on allocation failure.
  static void unavailable(observation::Observer observer,
                          IdentitySource &identities, std::string_view source,
                          std::string_view reason) noexcept {
    if (observer.on_fact == nullptr)
      return;
    const auto id = identities.next_fact(Producer::pack);
    if (!id)
      return;
    const std::array fields{known("source", source), known("reason", reason),
                            known("inventory_complete", false)};
    observer.observe(observation::FactView{
        *id,
        "pack_catalog_unavailable",
        {observation::Availability::not_applicable, 0, "catalog_preparation"},
        {},
        {},
        fields});
  }

  [[nodiscard]] bool complete() const noexcept { return complete_; }

  // Called only after the real assignment map was updated. The parsed index
  // is not a source ordinal: rejected rows have been removed from that index.
  std::optional<observation::FactId> loaded(std::size_t parsed_index,
                                            const AytherEventSub &assignment,
                                            bool replaced) noexcept {
    const auto *row = accepted_row(parsed_index, assignment.signature);
    Seen *identity = nullptr;
    for (std::size_t i = 0; i < seen_count_; ++i) {
      if (seen_[i].signature == assignment.signature) {
        identity = &seen_[i];
        break;
      }
    }
    const auto previous =
        identity ? identity->last_loaded : std::optional<FactId>{};
    if (replaced && !previous)
      complete_ = false;
    const std::array fields{
        row ? known("ordinal", row->ordinal, observation::Unit::count)
            : unknown("ordinal", "parser_load_link_unavailable"),
        known("signature", assignment.signature),
        known("asset", std::string_view{assignment.asset}),
        known("stage", std::string_view{"session_assignment_map"}),
        known("accepted", true),
        known("reason",
              std::string_view{replaced ? "replaced_existing_signature"
                                        : "inserted"}),
        known("channels", static_cast<std::uint64_t>(assignment.channels)),
        known("parsed_duration",
              static_cast<std::uint64_t>(assignment.duration_frames),
              observation::Unit::emulation_frame),
        known("parsed_span", static_cast<std::uint64_t>(assignment.span_frames),
              observation::Unit::emulation_frame),
        known("parsed_looping", assignment.looping != 0),
        row ? known("authored_asset_bytes", row->asset_bytes,
                    observation::Unit::bytes)
            : unknown("authored_asset_bytes", "parser_load_link_unavailable"),
        known("effective_asset_bytes",
              static_cast<std::uint64_t>(
                  std::string_view{assignment.asset}.size()),
              observation::Unit::bytes)};
    std::array<observation::Cause, 2> causes{};
    std::size_t count = 0;
    if (row && row->parsed)
      causes[count++] = *row->parsed;
    if (replaced && previous)
      causes[count++] = *previous;
    const auto result =
        emit("pack_assignment_loaded", fields, std::span{causes}.first(count));
    if (identity)
      identity->last_loaded = result;
    ++loaded_entries_;
    return result;
  }

  struct AssetResult {
    std::size_t parsed_index = 0;
    std::uint64_t signature = 0;
    std::optional<observation::FactId> load_fact;
    bool attempted_now = false;
    std::string_view attempt_reason;
    std::string_view source;
    bool cached = false;
    bool ready = false;
    std::uint64_t pcm_bytes = 0;
    std::string_view reason;
  };

  void asset_result(const AssetResult &result) noexcept {
    const auto *row = accepted_row(result.parsed_index, result.signature);
    const std::array fields{
        row ? known("ordinal", row->ordinal, observation::Unit::count)
            : unknown("ordinal", "parser_load_link_unavailable"),
        known("signature", result.signature),
        known("stage", std::string_view{"asset_prewarm"}),
        known("attempted_now", result.attempted_now),
        known("attempt_reason", result.attempt_reason),
        result.attempted_now
            ? known("source", result.source)
            : unknown("source", "no_source_selection_in_this_attempt"),
        known("cache_present", result.cached),
        result.cached ? known("ready", result.ready)
                      : unknown("ready", "not_cached"),
        result.cached
            ? known("pcm_bytes", result.pcm_bytes, observation::Unit::bytes)
            : unknown("pcm_bytes", "not_cached"),
        known("reason", result.reason)};
    const std::array<observation::Cause, 1> causes{
        result.load_fact.value_or(FactId{})};
    if (!result.load_fact)
      complete_ = false;
    emit("pack_assignment_asset_result", fields,
         result.load_fact ? std::span<const observation::Cause>{causes}
                          : std::span<const observation::Cause>{});
  }

  void finish_load(std::size_t distinct_assignments) noexcept {
    const std::array fields{
        known("stage", std::string_view{"session_assignment_map"}),
        known("assignment_updates", loaded_entries_, observation::Unit::count),
        known("distinct_loaded_assignments",
              static_cast<std::uint64_t>(distinct_assignments),
              observation::Unit::count),
        known("load_observation_complete", complete_)};
    const std::array<observation::Cause, 1> causes{
        inventory_.value_or(FactId{})};
    emit("pack_assignment_load_end", fields,
         inventory_ ? std::span<const observation::Cause>{causes}
                    : std::span<const observation::Cause>{});
  }

  static void callback(void *context,
                       const AytherAudioCatalogObservation *event) noexcept {
    if (context != nullptr && event != nullptr) {
      static_cast<CatalogRecorder *>(context)->accept(*event);
    }
  }

private:
  using Field = observation::FieldView;
  using FactId = observation::FactId;

  struct Accepted {
    std::uint64_t ordinal = 0;
    std::uint64_t signature = 0;
    std::uint64_t asset_bytes = 0;
    std::optional<FactId> parsed;
  };

  const Accepted *accepted_row(std::size_t index,
                               std::uint64_t signature) noexcept {
    if (index >= accepted_count_ || index >= accepted_.size() ||
        accepted_[index].signature != signature) {
      complete_ = false;
      return nullptr;
    }
    return &accepted_[index];
  }

  static Field
  known(std::string_view name, observation::Value value,
        observation::Unit unit = observation::Unit::none) noexcept {
    return {name, observation::Availability::known, unit, value, {}};
  }

  static Field unknown(std::string_view name,
                       std::string_view reason) noexcept {
    return {name, observation::Availability::unknown, observation::Unit::none,
            std::monostate{}, reason};
  }

  Field text(std::string_view name, const std::uint8_t *data, std::size_t size,
             bool declared) noexcept {
    if (!declared) {
      return unknown(name, "not_declared_as_text");
    }
    if (size > observation::max_text_bytes) {
      complete_ = false;
      return unknown(name, "text_limit");
    }
    return known(name,
                 std::string_view{reinterpret_cast<const char *>(data), size});
  }

  std::optional<FactId>
  emit(std::string_view kind, std::span<const Field> fields,
       std::span<const observation::Cause> causes = {}) noexcept {
    const auto id = identities_.next_fact(Producer::pack);
    if (!id) {
      complete_ = false;
      return {};
    }
    observer_.observe(observation::FactView{
        *id,
        kind,
        {observation::Availability::not_applicable, 0, "catalog_preparation"},
        causes,
        {},
        fields});
    return id;
  }

  void declared(const AytherAudioCatalogObservation &event) noexcept {
    Field duplicate = unknown("duplicate_of_ordinal", "invalid_signature");
    if (event.signature_known != 0) {
      bool found = false;
      for (std::size_t i = 0; i < seen_count_; ++i) {
        if (seen_[i].signature == event.signature) {
          duplicate = known("duplicate_of_ordinal", seen_[i].ordinal);
          ++duplicates_;
          found = true;
          break;
        }
      }
      if (!found && seen_count_ < seen_.size()) {
        seen_[seen_count_++] = {event.signature, event.ordinal, {}};
        duplicate = {
            "duplicate_of_ordinal", observation::Availability::not_applicable,
            observation::Unit::none, std::monostate{}, "first_declaration"};
      } else if (!found) {
        duplicate = unknown("duplicate_of_ordinal", "signature_tracking_limit");
        complete_ = false;
      }
    }
    const std::array fields{
        known("ordinal", event.ordinal, observation::Unit::count),
        text("authored_signature", event.signature_text, event.signature_bytes,
             event.signature_declared != 0),
        event.signature_known != 0 ? known("signature", event.signature)
                                   : unknown("signature", "invalid_signature"),
        text("asset", event.asset, event.asset_bytes,
             event.asset_declared != 0),
        known("signature_bytes",
              static_cast<std::uint64_t>(event.signature_bytes),
              observation::Unit::bytes),
        known("asset_bytes", static_cast<std::uint64_t>(event.asset_bytes),
              observation::Unit::bytes),
        duplicate};
    const std::array<observation::Cause, 1> causes{
        inventory_.value_or(FactId{})};
    last_entry_ = emit("pack_assignment_declared", fields,
                       inventory_ ? std::span<const observation::Cause>{causes}
                                  : std::span<const observation::Cause>{});
    last_ordinal_ = event.ordinal;
    last_asset_bytes_ = event.asset_bytes;
    ++declarations_;
  }

  static std::string_view reason(std::uint32_t code) noexcept {
    switch (code) {
    case 0:
      return "accepted_by_parser";
    case 1:
      return "invalid_toml";
    case 2:
      return "missing_event_array";
    case 3:
      return "invalid_signature";
    case 4:
      return "missing_asset";
    case 5:
      return "null_input";
    default:
      return "unknown_parser_reason";
    }
  }

  void accept(const AytherAudioCatalogObservation &event) noexcept {
    if (event.kind == 1) {
      const std::array fields{
          known("source", source_),
          known("declared_entries", event.count, observation::Unit::count)};
      inventory_ = emit("pack_catalog_begin", fields);
    } else if (event.kind == 2) {
      declared(event);
    } else if (event.kind == 3 || event.kind == 4) {
      const std::array fields{
          known("ordinal", event.ordinal, observation::Unit::count),
          known("accepted", event.kind == 3),
          known("stage", std::string_view{"catalog_parser"}),
          known("reason", reason(event.reason))};
      const std::array<observation::Cause, 1> causes{
          last_entry_.value_or(FactId{})};
      const bool linked = last_entry_ && last_ordinal_ == event.ordinal;
      if (!linked)
        complete_ = false;
      const auto parsed =
          emit("pack_assignment_parse_result", fields,
               linked ? std::span<const observation::Cause>{causes}
                      : std::span<const observation::Cause>{});
      if (event.kind == 3) {
        if (accepted_count_ < accepted_.size()) {
          accepted_[accepted_count_] = {event.ordinal, event.signature,
                                        last_asset_bytes_, parsed};
        } else {
          complete_ = false;
        }
        ++accepted_count_;
      }
    } else if (event.kind == 5) {
      const std::array fields{known("parser_accepted_entries", event.count,
                                    observation::Unit::count),
                              known("observed_declarations", declarations_,
                                    observation::Unit::count),
                              known("duplicate_declarations", duplicates_,
                                    observation::Unit::count),
                              known("inventory_complete", complete_)};
      const std::array<observation::Cause, 1> causes{
          inventory_.value_or(FactId{})};
      emit("pack_catalog_end", fields,
           inventory_ ? std::span<const observation::Cause>{causes}
                      : std::span<const observation::Cause>{});
    } else if (event.kind == 6) {
      complete_ = false;
      const std::array fields{known("source", source_),
                              known("reason", reason(event.reason)),
                              known("inventory_complete", false)};
      inventory_ = emit("pack_catalog_unavailable", fields);
    } else {
      complete_ = false;
    }
  }

  struct Seen {
    std::uint64_t signature = 0;
    std::uint64_t ordinal = 0;
    std::optional<FactId> last_loaded;
  };
  observation::Observer observer_;
  IdentitySource &identities_;
  std::string_view source_;
  std::array<Seen, 4096> seen_{};
  std::size_t seen_count_ = 0;
  std::array<Accepted, 4096> accepted_{};
  std::size_t accepted_count_ = 0;
  std::optional<FactId> inventory_;
  std::optional<FactId> last_entry_;
  std::uint64_t last_ordinal_ = 0;
  std::uint64_t last_asset_bytes_ = 0;
  std::uint64_t loaded_entries_ = 0;
  std::uint64_t declarations_ = 0;
  std::uint64_t duplicates_ = 0;
  bool complete_ = true;
};

static_assert(sizeof(CatalogRecorder) <= std::size_t{512} * 1024);

} // namespace ayther::audio_qa
