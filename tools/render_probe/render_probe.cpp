// render_probe (spec 002, plan §5.13) - reproduces a take without a window
// with the full pipeline: the session replays the take and the Vulkan
// renderer composes every frame offscreen.
//
//   render_probe --rom R --core C --take T (--pack P | --no-pack)
//                --frames a-b --out D [--shaders S]
//
// Writes, per frame f of [a, b]: D/frame_<f>.json, the frame record of plan
// §4.3 with the render observation (contracts.md C3); D/composed_<f>.png,
// the renderer's image; and D/core_<f>.png, the core's framebuffer. The
// pack's overlays (Acetatos) are stacked at their authored positions and
// drawn, and every record lists them with their gate (DI-18).
//
// --check o3 (spec 002, DI-17) checks continuity: from one frame to the next
// the composed image may only change where the core's image changes too. The
// result goes to every record and to D/o3.csv. With --no-images the PNGs are
// not written and only the records of frames with a finding are, so a whole
// take can be scanned.
//
// With --timing it writes no images or records: it times every frame of
// [a, b] (produce + compose, probe_timing.h) into D/timing.csv and prints the
// p50/p95/max summary (plan §8, P-8).
#include "ayther_layers.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "ayther_session.h"
#include "probe_images.h"
#include "probe_json.h"
#include "probe_missing.h"
#include "probe_o2.h"
#include "probe_o3.h"
#include "probe_timing.h"
#include "vulkan_test_context.h"

#include <ayther/engine/render_observer.hpp>

#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
namespace ro = ayther::engine::render_observation;

struct Options {
  std::string rom;
  std::string core;
  std::string take;
  std::string pack;
  std::string trust_registry;
  bool no_pack = false;
  std::uint32_t first = 0;
  std::uint32_t last = 0;
  bool frames_set = false;
  bool check_o1 = false;
  bool settle = false;
  bool timing = false;
  bool prewarm = false;
  std::vector<std::pair<std::string, std::string>> core_options;
  bool check_o2 = false;
  bool check_o3 = false;
  bool no_images = false;
  fs::path out;
  std::string shaders = std::string(AYTHER_SOURCE_DIR) + "/shaders/";
};

void usage() {
  std::fprintf(stderr,
               "usage: render_probe --rom R --core C --take T "
               "(--pack P | --no-pack) --frames a-b --out D [--shaders S] "
               "[--check o1|o2|o3]... [--core-option KEY=VALUE]... "
               "[--trust-registry TOML] [--settle] [--timing] [--prewarm] "
               "[--no-images]\n");
}

std::optional<Options> parse(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&]() -> const char * {
      return i + 1 < argc ? argv[++i] : nullptr;
    };
    if (arg == "--no-pack") {
      o.no_pack = true;
      continue;
    }
    if (arg == "--settle") {
      o.settle = true;
      continue;
    }
    if (arg == "--timing") {
      o.timing = true;
      continue;
    }
    if (arg == "--prewarm") {
      o.prewarm = true;
      continue;
    }
    if (arg == "--no-images") {
      o.no_images = true;
      continue;
    }
    const char *v = value();
    if (v == nullptr)
      return std::nullopt;
    if (arg == "--rom")
      o.rom = v;
    else if (arg == "--core")
      o.core = v;
    else if (arg == "--take")
      o.take = v;
    else if (arg == "--pack")
      o.pack = v;
    else if (arg == "--trust-registry")
      o.trust_registry = v;
    else if (arg == "--out")
      o.out = v;
    else if (arg == "--shaders")
      o.shaders = v;
    else if (arg == "--core-option") {
      const std::string kv = v;
      const std::size_t eq = kv.find('=');
      if (eq == std::string::npos || eq == 0)
        return std::nullopt;
      o.core_options.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (arg == "--check") {
      const std::string_view check = v;
      if (check == "o1")
        o.check_o1 = true;
      else if (check == "o2")
        o.check_o2 = true;
      else if (check == "o3")
        o.check_o3 = true;
      else
        return std::nullopt;
    } else if (arg == "--frames") {
      const std::string text = v;
      const std::size_t dash = text.find('-');
      std::uint32_t a = 0;
      std::uint32_t b = 0;
      if (dash == std::string::npos ||
          std::from_chars(text.data(), text.data() + dash, a).ec !=
              std::errc{} ||
          std::from_chars(text.data() + dash + 1, text.data() + text.size(), b)
                  .ec != std::errc{} ||
          b < a)
        return std::nullopt;
      o.first = a;
      o.last = b;
      o.frames_set = true;
    } else {
      return std::nullopt;
    }
  }
  if (o.rom.empty() || o.core.empty() || o.take.empty() || o.out.empty() ||
      !o.frames_set || (o.pack.empty() == !o.no_pack))
    return std::nullopt;
  return o;
}

// A stable identity for a sprite across frames: its SAT slot and content.
std::uint32_t sprite_identity(std::uint8_t slot, std::uint64_t hash) {
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(slot) << 24U) ^
                                    (hash & 0xFFFFFFU) ^ (hash >> 40U));
}

// The O2 view of one frame (plan §5.13), from the observation, the frame's
// scene and how the renderer drew it.
ayther::probe::O2Frame o2_frame(const ro::RenderFrameView &view,
                                const ayther::FrameView &fv) {
  ayther::probe::O2Frame out;
  const bool composed =
      fv.scene != nullptr && fv.scene_count > 0 && fv.scene_dirty == 0;
  std::vector<bool> used(fv.scene != nullptr ? fv.scene_count : 0, false);
  std::vector<std::uint32_t> id_of(view.occurrences.size(), 0);
  for (std::size_t i = 0; i < view.occurrences.size(); ++i) {
    const ro::OccurrenceView &o = view.occurrences[i];
    ayther::probe::O2Occurrence occ;
    occ.index = sprite_identity(o.id.slot, o.identity_hash);
    id_of[i] = occ.index;
    occ.depth = o.id.chain;
    // The scene element that draws this occurrence's original.
    const ayther::SceneElement *element = nullptr;
    for (std::uint32_t e = 0; composed && e < fv.scene_count; ++e)
      if (!used[e] && fv.scene[e].layer == 3 && fv.scene[e].slot == o.id.slot &&
          fv.scene[e].x == o.x && fv.scene[e].y == o.y) {
        used[e] = true;
        element = &fv.scene[e];
        break;
      }
    occ.claimed = element != nullptr
                      ? element->claimed != 0
                      : o.status == ro::OccurrenceStatus::replaced;
    occ.original_drawn =
        composed ? element != nullptr && element->claimed == 0 &&
                       element->hidden == 0
                 : true; // a frame that is not composed shows the core's image
    if (element != nullptr)
      occ.depth = element->chain;
    out.occurrences.push_back(occ);
  }
  for (const ro::ReplacementView &r : view.replacements) {
    ayther::probe::O2Replacement rep;
    rep.index = r.index;
    std::uint64_t key = 0xCBF29CE484222325ULL;
    for (const char c : r.asset)
      key = (key ^ static_cast<unsigned char>(c)) * 0x100000001B3ULL;
    for (const char c : r.pose_key)
      key = (key ^ static_cast<unsigned char>(c)) * 0x100000001B3ULL;
    rep.key = key;
    const bool known = r.render_availability == ro::Availability::known;
    rep.drawn = known && r.draw != ro::DrawOutcome::discarded;
    rep.texture_ready = known && r.texture == ro::TextureState::ready;
    // In the scene pass a replacement is drawn whole at its anchor's depth;
    // in a lane it is drawn in front of every sprite (depth 0).
    std::uint8_t depth = 0;
    if (known && r.draw == ro::DrawOutcome::in_pass)
      for (std::uint32_t e = 0; fv.scene != nullptr && e < fv.scene_count; ++e)
        if (fv.scene[e].layer == 3 && fv.scene[e].sub_kind == 1 &&
            fv.scene[e].sub == static_cast<std::int32_t>(r.index)) {
          depth = fv.scene[e].chain;
          break;
        }
    for (const ro::OccurrenceId &m : r.members) {
      if (m.index < id_of.size())
        rep.members.push_back(id_of[m.index]);
      if (!rep.drawn)
        continue;
      // Spec 002 (R2): drawn by parts, a member's partition is a part over
      // its rectangle at its own chain. A member hidden whole behind members
      // in front of it has none and is at its depth by construction; any
      // other takes the depth of the part over its centre.
      std::uint8_t member_depth = depth;
      if (r.draw == ro::DrawOutcome::partitioned &&
          m.index < view.occurrences.size()) {
        const ro::OccurrenceView &o = view.occurrences[m.index];
        const int cx = o.x + o.w / 2;
        const int cy = o.y + o.h / 2;
        bool own = false;
        bool behind_front = true; // every overlapping part is a front member
        std::uint8_t at_centre = depth;
        for (std::uint32_t k = 0;
             fv.sprite_partitions && k < fv.sprite_partition_count; ++k) {
          const ayther::SpritePartition &pt = fv.sprite_partitions[k];
          if (pt.sub != r.index)
            continue;
          const bool overlaps = pt.x < o.x + o.w && o.x < pt.x + pt.w &&
                                pt.y < o.y + o.h && o.y < pt.y + pt.h;
          own = own || (overlaps && pt.chain == o.id.chain);
          behind_front = behind_front && (!overlaps || pt.chain <= o.id.chain);
          if (cx >= pt.x && cx < pt.x + pt.w && cy >= pt.y && cy < pt.y + pt.h)
            at_centre = pt.chain;
        }
        member_depth = own || behind_front ? o.id.chain : at_centre;
      }
      rep.partition_depths.push_back(member_depth);
    }
    out.replacements.push_back(rep);
  }
  return out;
}

const char *invariant_name(ayther::probe::O2Invariant i) {
  using I = ayther::probe::O2Invariant;
  switch (i) {
  case I::claimed_in_applied:
    return "claimed_in_applied";
  case I::claimed_drawn_resident:
    return "claimed_drawn_resident";
  case I::partition_depth:
    return "partition_depth";
  case I::unclaimed_drawn:
    return "unclaimed_drawn";
  case I::no_double:
    return "no_double";
  case I::transition:
    return "transition";
  }
  return "transition";
}

// The record of the last published frame (plan §4.3), and its O2 view.
class JsonObserver final : public ro::RenderObserver {
public:
  std::uint32_t frame = 0;
  std::uint64_t emulation_frame = 0;
  const ayther::FrameView *fv = nullptr;
  std::string json;
  ayther::probe::O2Frame o2;
  void on_render_frame(const ro::RenderFrameView &view) noexcept override {
    try {
      json = ayther::probe::frame_json(frame, emulation_frame, view);
      if (fv != nullptr)
        o2 = o2_frame(view, *fv);
    } catch (...) {
      json.clear();
    }
  }
};

bool write_png(const fs::path &path, const ayther::probe::RgbImage &image) {
  return stbi_write_png(path.string().c_str(), static_cast<int>(image.width),
                        static_cast<int>(image.height), 3, image.rgb.data(),
                        static_cast<int>(image.width) * 3) != 0;
}

// Adds `field` (",\n  \"name\": value") at the end of a frame record.
std::string with_field(std::string record, const std::string &field) {
  const std::size_t end = record.rfind('}');
  if (end != std::string::npos)
    record.insert(record.find_last_not_of("\n ", end - 1) + 1, field);
  return record;
}

// Spec 002 (DI-18): the pack's overlays in the layer stack, each at the
// position it was authored at (in increasing order, so every position counts
// the overlays already placed), the ones without a position on top in pack
// order — as a frontend that draws the pack must stack them.
void stack_pack_overlays(
    const std::vector<ayther::AytherSession::PackOverlay> &overlays,
    AytherLayerStack &stack) {
  std::vector<const ayther::AytherSession::PackOverlay *> ordered;
  for (const auto &overlay : overlays)
    ordered.push_back(&overlay);
  std::stable_sort(
      ordered.begin(), ordered.end(),
      [](const auto *a, const auto *b) { return a->index < b->index; });
  for (const auto *overlay : ordered) {
    const std::size_t at = (std::min)(static_cast<std::size_t>(overlay->index),
                                      stack.layers().size());
    const std::uint32_t id = stack.insert_custom(overlay->name.c_str(), at);
    if (id == 0)
      continue;
    (void)stack.set_visible(id, overlay->visible);
    (void)stack.set_content(id, overlay->content);
  }
}

bool write_text(const fs::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

} // namespace

int main(int argc, char **argv) try {
  const std::optional<Options> parsed = parse(argc, argv);
  if (!parsed) {
    usage();
    return 2;
  }
  const Options &o = *parsed;
  std::error_code ec;
  fs::create_directories(o.out, ec);

  auto recording = ayther::AytherRecording::load(o.take);
  if (!recording || o.last >= recording->frame_count()) {
    std::fprintf(stderr, "render_probe: cannot use take %s for frames %u-%u\n",
                 o.take.c_str(), o.first, o.last);
    return 1;
  }

  JsonObserver observer;
  ayther::AytherSession::Config config;
  config.core_path = o.core;
  config.rom_path = o.rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.render_observer = &observer;
  config.core_options = o.core_options;
  // A signed production pack is opened through the registry that vouches
  // for it, as Play does; without one the authoring path is used.
  config.pack_path = o.pack;
  config.trust_registry = o.trust_registry;
  auto created = ayther::AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "render_probe: %s\n", created.error.message.c_str());
    return 1;
  }
  std::unique_ptr<ayther::AytherSession> session = std::move(*created);
  if (!o.pack.empty() && !session->pack().is_valid()) {
    std::fprintf(stderr, "render_probe: cannot open pack %s\n", o.pack.c_str());
    return 1;
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "render_probe: SDL_Init failed\n");
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("render_probe", 64, 64,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  VulkanTestContext ctx;
  if (window == nullptr || !ctx.init(window)) {
    std::fprintf(stderr, "render_probe: no Vulkan context\n");
    return 1;
  }
  ayther::AytherRenderer renderer;
  AytherLayerStack stack;
  stack_pack_overlays(session->pack_overlays(), stack);
  // --prewarm (spec 002, BR-091): the preparation prewarms the pack
  // catalog before the first frame, at the take's first frame size.
  std::uint32_t warm_w = 0;
  std::uint32_t warm_h = 0;
  if (o.prewarm) {
    const ayther::FrameView *fv = session->replay_seek(*recording, o.first);
    if (fv == nullptr || fv->fb_width == 0 ||
        !renderer.init(ctx, fv->fb_width, fv->fb_height, o.shaders.c_str()) ||
        !renderer.readback_init(ctx)) {
      std::fprintf(stderr, "render_probe: cannot prepare the renderer\n");
      return 1;
    }
    warm_w = fv->fb_width;
    warm_h = fv->fb_height;
    const std::vector<std::string> assets = session->catalog_texture_assets();
    const ayther::AytherRenderer::PrewarmReport r =
        renderer.prewarm_textures(ctx, session->pack(), assets);
    std::printf("render_probe prewarm assets=%u resident=%u over_budget=%u "
                "failed=%u gpu_mib=%.1f decode_ms=%.1f upload_ms=%.1f\n",
                r.assets, r.resident, r.over_budget, r.failed,
                static_cast<double>(r.gpu_bytes) / (1024.0 * 1024.0),
                r.decode_ms, r.upload_ms);
  }
  if (o.timing) {
    std::vector<ayther::probe::FrameTiming> rows;
    const bool timed = ayther::probe::time_frames(
        *session, *recording, renderer, ctx, stack, o.shaders.c_str(), o.first,
        o.last, rows, warm_w, warm_h);
    const ayther::probe::TimingSummary sum = ayther::probe::summarize(rows);
    std::printf("render_probe timing frames=%zu p50_ms=%.3f p95_ms=%.3f "
                "max_ms=%.3f\n",
                sum.frames, sum.p50_ms, sum.p95_ms, sum.max_ms);
    const bool written =
        ayther::probe::write_timing_csv((o.out / "timing.csv").string(), rows);
    ctx.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return timed && written ? 0 : 1;
  }
  std::uint32_t width = warm_w;
  std::uint32_t height = warm_h;
  int status = 0;
  ayther::probe::O2Frame previous_o2;
  bool have_previous = false;
  // O3: the previous frame's images, and one row per frame for o3.csv.
  ayther::probe::RgbImage previous_composed;
  ayther::probe::RgbImage previous_core;
  bool have_previous_images = false;
  std::string o3_csv = "frame,unexplained_blocks,y0,y1,composability,"
                       "raster_reasons,raster_bands\n";
  std::uint32_t o3_frames = 0;

  for (std::uint32_t f = o.first; f <= o.last; ++f) {
    const ayther::FrameView *fv = session->replay_seek(*recording, f);
    if (fv == nullptr || fv->fb_width == 0 || fv->fb_height == 0) {
      std::fprintf(stderr, "render_probe: frame %u did not reproduce\n", f);
      status = 1;
      break;
    }
    if (fv->fb_width != width || fv->fb_height != height) {
      // A mode change resizes the renderer, as a host does: its texture
      // caches (and what --prewarm loaded) survive it.
      const bool first = width == 0;
      if (!first)
        renderer.readback_shutdown(ctx);
      width = fv->fb_width;
      height = fv->fb_height;
      const bool ready =
          first ? renderer.init(ctx, width, height, o.shaders.c_str())
                : renderer.resize(ctx, width, height);
      if (!ready || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "render_probe: renderer init failed\n");
        status = 1;
        break;
      }
    }
    const std::uint8_t *pixels = renderer.export_frame(
        ctx, *fv, session->pack(), /*hd_on=*/true, &stack);
    if (pixels == nullptr) {
      std::fprintf(stderr, "render_probe: frame %u did not render\n", f);
      status = 1;
      break;
    }
    // --settle: textures load asynchronously, so a cold probe sees them
    // pending. Rendering the same frame again until none is pending (bounded)
    // shows the frame with its replacements resident.
    std::uint32_t settle_renders = 0;
    const auto pending = [&] {
      for (const ro::ReplacementDraw &row :
           renderer.last_draw_report().replacements)
        if (row.texture == ro::TextureState::pending &&
            row.draw != ro::DrawOutcome::discarded)
          return true;
      return false;
    };
    while (o.settle && pixels != nullptr && pending() && settle_renders < 400) {
      SDL_Delay(2);
      pixels = renderer.export_frame(ctx, *fv, session->pack(),
                                     /*hd_on=*/true, &stack);
      ++settle_renders;
    }
    if (pixels == nullptr) {
      status = 1;
      break;
    }
    const ayther::probe::RgbImage composed =
        ayther::probe::readback_image(pixels, width, height);
    const ayther::probe::RgbImage core =
        ayther::probe::core_image(fv->fb_pixels, fv->fb_width, fv->fb_height,
                                  fv->fb_pitch, fv->fb_format);
    char png[40];
    bool written = true;
    if (!o.no_images) {
      std::snprintf(png, sizeof(png), "composed_%06u.png", f);
      written = write_png(o.out / png, composed);
      std::snprintf(png, sizeof(png), "core_%06u.png", f);
      written = written && write_png(o.out / png, core);
    }
    if (!written) {
      std::fprintf(stderr, "render_probe: cannot write PNGs of frame %u\n", f);
      status = 1;
      break;
    }
    const ro::DrawReport report = renderer.last_draw_report();
    // Spec 002 (BR-093, RF-2.11): replay QA stops on an assigned asset that
    // cannot be read, with the diagnostic, after writing this frame's record.
    const std::optional<ayther::probe::MissingAsset> missing =
        ayther::probe::first_missing_asset(
            report,
            {fv->sprite_subs, fv->sprite_subs ? fv->sprite_sub_count : 0u});
    observer.frame = f;
    observer.emulation_frame = fv->frame_index;
    observer.json.clear();
    observer.fv = fv;
    session->publish_render_observation(&report);
    std::string json = observer.json;
    if (o.settle)
      json = with_field(json, ",\n  \"settle_renders\": " +
                                  std::to_string(settle_renders));
    // Spec 002 (DI-17): the core's raster reasons and the bands they leave
    // non-composable.
    if (fv->raster_reasons != 0) {
      std::string field =
          ",\n  \"raster\": {\"reasons\": " +
          std::to_string(fv->raster_reasons) + ", \"localized\": " +
          ((fv->scene_dirty & 1U) == 0 ? "true" : "false") + ", \"bands\": [";
      for (std::uint32_t k = 0; k < fv->raster_band_count; ++k)
        field += std::string(k ? ", [" : "[") +
                 std::to_string(fv->raster_bands[k][0]) + ", " +
                 std::to_string(fv->raster_bands[k][1]) + "]";
      field += "]}";
      json = with_field(json, field);
    }
    bool finding = false; // written with --no-images too
    if (o.check_o1) {
      // O1: without pack and shaders, at native resolution, the composed
      // image must be the core's framebuffer pixel for pixel.
      const ayther::probe::O1Result r = ayther::probe::check_o1(composed, core);
      json = with_field(
          json,
          ",\n  \"o1\": {\"identical\": " +
              std::string(r.identical ? "true" : "false") +
              ", \"differing_pixels\": " + std::to_string(r.differing_pixels) +
              ", \"first_x\": " + std::to_string(r.first_x) +
              ", \"first_y\": " + std::to_string(r.first_y) + "}");
      std::printf("render_probe o1 frame=%u identical=%d differing=%zu\n", f,
                  r.identical ? 1 : 0, r.differing_pixels);
    }
    if (o.check_o2) {
      // O2: the six structural invariants of plan §5.13.
      const std::vector<ayther::probe::O2Violation> violations =
          ayther::probe::check_o2(observer.o2,
                                  have_previous ? &previous_o2 : nullptr);
      std::string field = ",\n  \"o2\": {\"violations\": [";
      for (std::size_t v = 0; v < violations.size(); ++v) {
        field += v == 0 ? "" : ", ";
        field += "{\"invariant\": \"";
        field += invariant_name(violations[v].invariant);
        field +=
            "\", \"subject\": " + std::to_string(violations[v].subject) + "}";
      }
      field += "]}";
      json = with_field(json, field);
      std::printf("render_probe o2 frame=%u violations=%zu\n", f,
                  violations.size());
      finding = finding || !violations.empty();
      previous_o2 = observer.o2;
      have_previous = true;
    }
    if (o.check_o3) {
      // O3: continuity with the previous frame of the range.
      ayther::probe::O3Result r;
      if (have_previous_images)
        r = ayther::probe::check_o3(previous_composed, composed, previous_core,
                                    core);
      json = with_field(json, ",\n  \"o3\": {\"unexplained_blocks\": " +
                                  std::to_string(r.unexplained_blocks) +
                                  ", \"y0\": " + std::to_string(r.y0) +
                                  ", \"y1\": " + std::to_string(r.y1) + "}");
      if (r.unexplained_blocks > 0) {
        finding = true;
        ++o3_frames;
        std::string bands;
        for (std::uint32_t k = 0; k < fv->raster_band_count; ++k)
          bands += (k ? " " : "") + std::to_string(fv->raster_bands[k][0]) +
                   "-" + std::to_string(fv->raster_bands[k][1]);
        const std::size_t c = json.find("\"composability\": \"");
        const std::string composability =
            c == std::string::npos
                ? std::string()
                : json.substr(c + 18, json.find('"', c + 18) - c - 18);
        o3_csv += std::to_string(f) + "," +
                  std::to_string(r.unexplained_blocks) + "," +
                  std::to_string(r.y0) + "," + std::to_string(r.y1) + "," +
                  composability + "," + std::to_string(fv->raster_reasons) +
                  "," + bands + "\n";
        std::printf("render_probe o3 frame=%u unexplained_blocks=%u "
                    "rows=%d-%d\n",
                    f, r.unexplained_blocks, r.y0, r.y1);
      }
      previous_composed = composed;
      previous_core = core;
      have_previous_images = true;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06u.json", f);
    if (missing)
      json = with_field(json,
                        ",\n  \"missing_asset\": \"" + missing->asset + "\"");
    if ((!o.no_images || finding || missing) &&
        !write_text(o.out / name, json)) {
      status = 1;
      break;
    }
    if (missing) {
      std::fprintf(stderr,
                   "render_probe: missing_asset %s (replacement %u) at frame "
                   "%u: the take stops\n",
                   missing->asset.c_str(), missing->replacement, f);
      status = 3;
      break;
    }
  }

  if (o.check_o3) {
    if (!write_text(o.out / "o3.csv", o3_csv))
      status = 1;
    std::printf("render_probe o3 frames=%u-%u discontinuous=%u\n", o.first,
                o.last, o3_frames);
  }
  if (width != 0) {
    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
  }
  ctx.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return status;
} catch (const std::exception &error) {
  std::fprintf(stderr, "render_probe: %s\n", error.what());
  return 1;
}
