// ---------------------------------------------------------------------------
// ayther_audio_events.cpp — AudioEventSubstitution (see ayther_audio_events.h).
//
// STATUS: contract/skeleton, NOT yet wired into the root CMakeLists.txt. Depends
// only on the ayther_audio_sub_* / ayther_audio_evdet_* FFI (done + tested in
// C-A1) and std — no SDL/Vulkan — so it type-checks standalone:
//   clang-cl /std:c++20 /Zs -I include/ayther src/ayther_audio_events.cpp
// Left out of the engine build until produce_frame() drives it (and AudioPlayer
// grows play_event_hd() with an OGG decoder).
// ---------------------------------------------------------------------------

#include "ayther_audio_events.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ayther {

struct AudioEventSubstitution::Impl {
    AytherAudioSubstitutor* sub = nullptr;                 ///< owns the event override catalog

    // signature → (asset, looping). Mirrors the Rust catalog so we can enumerate
    // (for Deliver) and rebuild on unassign (no single-remove FFI).
    std::map<uint64_t, std::pair<std::string, bool>> assigns;

    std::vector<AytherAudioEventSub> subs;                 ///< resolved windows, sorted by start_frame

    ~Impl() { if (sub) ayther_audio_sub_free(sub); }

    void reapply() {
        ayther_audio_sub_clear_event_overrides(sub);
        for (const auto& [sig, av] : assigns)
            ayther_audio_sub_add_event_override(sub, sig, av.first.c_str(), av.second ? 1 : 0);
    }
};

AudioEventSubstitution::AudioEventSubstitution() : impl_(std::make_unique<Impl>()) {
    impl_->sub = ayther_audio_sub_new();
}
AudioEventSubstitution::~AudioEventSubstitution() = default;
AudioEventSubstitution::AudioEventSubstitution(AudioEventSubstitution&&) noexcept            = default;
AudioEventSubstitution& AudioEventSubstitution::operator=(AudioEventSubstitution&&) noexcept = default;

void AudioEventSubstitution::assign(uint64_t signature, const std::string& asset, bool looping) {
    if (asset.empty()) { unassign(signature); return; }
    impl_->assigns[signature] = { asset, looping };
    ayther_audio_sub_add_event_override(impl_->sub, signature, asset.c_str(), looping ? 1 : 0);
}

void AudioEventSubstitution::unassign(uint64_t signature) {
    if (impl_->assigns.erase(signature)) impl_->reapply();   // rebuild (no single-remove FFI)
}

void AudioEventSubstitution::clear() noexcept {
    impl_->assigns.clear();
    ayther_audio_sub_clear_event_overrides(impl_->sub);
    impl_->subs.clear();
}

bool AudioEventSubstitution::has_assignments() const noexcept {
    return !impl_->assigns.empty();
}

std::vector<AudioEventAssignment> AudioEventSubstitution::assignments() const {
    std::vector<AudioEventAssignment> out;
    out.reserve(impl_->assigns.size());
    for (const auto& [sig, av] : impl_->assigns)          // std::map → sorted by signature
        out.push_back(AudioEventAssignment{ sig, av.first, av.second });
    return out;
}

void AudioEventSubstitution::resolve(const AytherAudioEvent* events, uint32_t event_count) {
    auto& im = *impl_;
    im.subs.clear();
    if (!events || event_count == 0 || im.assigns.empty()) return;

    im.subs.resize(event_count);                          // at most one sub per event
    const uint32_t n = ayther_audio_sub_resolve_events(
        im.sub, events, event_count, im.subs.data(), event_count);
    im.subs.resize(n);
    std::sort(im.subs.begin(), im.subs.end(),
              [](const AytherAudioEventSub& a, const AytherAudioEventSub& b) {
                  return a.start_frame < b.start_frame;
              });
}

bool AudioEventSubstitution::mute_at(uint64_t frame) const noexcept {
    for (const auto& s : impl_->subs)
        if (frame >= s.start_frame && frame <= s.end_frame) return true;
    return false;
}

uint32_t AudioEventSubstitution::triggers_at(uint64_t frame, AudioEventTrigger* out, uint32_t cap) const {
    if (!out) return 0;
    uint32_t k = 0;
    for (const auto& s : impl_->subs) {
        if (s.start_frame != frame) continue;
        if (k >= cap) break;
        out[k].asset     = s.asset_path;
        out[k].looping   = s.looping != 0;
        out[k].signature = s.signature;
        out[k].end_frame = s.end_frame;
        ++k;
    }
    return k;
}

uint32_t AudioEventSubstitution::sub_count() const noexcept {
    return static_cast<uint32_t>(impl_->subs.size());
}
const AytherAudioEventSub* AudioEventSubstitution::subs() const noexcept {
    return impl_->subs.data();
}

}  // namespace ayther
