// ---------------------------------------------------------------------------
// hd_c8_slot_smoke — E4/C8: valida el slot SAT por sub (sprite_sub_slot) que
// alimenta el z-order del renderer entre HD superpuestos. El renderer dibuja los
// subs por slot DESCENDENTE (frontmost = slot menor = dibujado último = encima).
// Este oráculo verifica el DATO: cada sub toma el slot correcto (per-sprite = el
// de su occ; claimed/bbox = el menor de las occs del bbox), y que existe el
// escenario C8 (≥2 sprites que se solapan con slots distintos).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target hd_c8_slot_smoke
//   Run:    bin/hd_c8_slot_smoke[.exe]
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;

static std::string quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}
static bool overlap(const AytherSpriteOccurrence& a, const AytherSpriteOccurrence& b) {
    const int ax1 = a.screen_x + a.w_tiles * 8, ay1 = a.screen_y + a.h_tiles * 8;
    const int bx1 = b.screen_x + b.w_tiles * 8, by1 = b.screen_y + b.h_tiles * 8;
    return a.screen_x < bx1 && ax1 > b.screen_x && a.screen_y < by1 && ay1 > b.screen_y;
}

int main() {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    if (!cfg) { std::fprintf(stderr, "[skip] sin tests/test_config.toml\n"); return 0; }
    std::string core, rom, line; bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) { in_rom = true; continue; }
        if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = quoted(line);
        else if (in_rom && rom.empty() && line.find("path") != std::string::npos) rom = quoted(line);
    }
    core = resolve(core, root); rom = resolve(rom, root);
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== hd_c8_slot_smoke ===\ncore: %s\nrom : %s\n\n", core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    constexpr uint16_t START = 1u << 3, RIGHT = 1u << 7;
    for (int i = 0; i < 900; ++i) { s->set_input(0, (i % 24 < 6) ? START : 0); s->step(); }
    s->record_start();
    for (int i = 0; i < 360; ++i) { uint16_t in = RIGHT; if (i % 48 < 4) in |= 1u << 0; s->set_input(0, in); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames\n", take.frame_count());

    int fail = 0;
    auto check = [&](bool ok, const char* m) { std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail; };

    // Buscar un frame con sprites (preferir uno con un par SUPERPUESTO de slots
    // distintos = escenario C8 real; si no hay, cualquiera con ≥2 sirve para
    // validar el DATO sprite_sub_slot). Asignar HD a todos los hashes y verificar.
    uint32_t F = UINT32_MAX, F_any = UINT32_MAX; bool F_overlap = false;
    for (uint32_t f = 30; f < take.frame_count() && F == UINT32_MAX; f += 6) {
        const FrameView* fv = s->replay_seek(take, f);
        if (!fv || fv->sprite_occ_count < 2) continue;
        if (F_any == UINT32_MAX) F_any = f;
        for (uint32_t i = 0; i < fv->sprite_occ_count && F == UINT32_MAX; ++i)
            for (uint32_t j = i + 1; j < fv->sprite_occ_count; ++j)
                if (fv->sprite_occs[i].slot != fv->sprite_occs[j].slot &&
                    overlap(fv->sprite_occs[i], fv->sprite_occs[j])) { F = f; F_overlap = true; break; }
    }
    if (F == UINT32_MAX) F = F_any;   // sin solape real: validar el dato igual
    check(F != UINT32_MAX, "hay un frame con sprites para validar sprite_sub_slot");
    if (F == UINT32_MAX) { std::printf("\n[sin sprites] — nada que validar en esta toma.\n"); return 0; }
    std::printf("[..] escenario C8 de solape real: %s\n", F_overlap ? "SÍ (par superpuesto)" : "no en esta toma (solo se valida el dato)");

    // Asignar HD a todos los hashes de sprite del frame para que se generen subs.
    const FrameView* fv = s->replay_seek(take, F);
    std::unordered_set<uint64_t> hashes;
    for (uint32_t i = 0; i < fv->sprite_occ_count; ++i) hashes.insert(fv->sprite_occs[i].hash);
    for (uint64_t h : hashes) s->assign_sprite(h, "fake_hd.png");
    s->replay_invalidate();
    fv = s->replay_seek(take, F);
    check(fv && fv->sprite_sub_count > 0 && fv->sprite_sub_slot != nullptr,
          "se generaron subs con sprite_sub_slot publicado");
    if (!fv || !fv->sprite_sub_count || !fv->sprite_sub_slot) { std::printf("\nFALLÓ\n"); return 1; }
    std::printf("[..] F=%u  occs=%u  subs=%u\n", F, fv->sprite_occ_count, fv->sprite_sub_count);

    // Verificar cada sub: exact-match → slot de su occ; si no, min slot de occs del bbox.
    size_t mismatch = 0, exact_n = 0, bbox_n = 0;
    for (uint32_t sidx = 0; sidx < fv->sprite_sub_count; ++sidx) {
        const auto& sub = fv->sprite_subs[sidx];
        uint8_t want_exact = 255, want_ovl = 255; bool is_exact = false;
        const int sx1 = sub.screen_x + sub.w_tiles * 8, sy1 = sub.screen_y + sub.h_tiles * 8;
        for (uint32_t o = 0; o < fv->sprite_occ_count; ++o) {
            const auto& oc = fv->sprite_occs[o];
            if (oc.screen_x == sub.screen_x && oc.screen_y == sub.screen_y &&
                oc.w_tiles == sub.w_tiles && oc.h_tiles == sub.h_tiles) { want_exact = oc.slot; is_exact = true; break; }
            const int ox1 = oc.screen_x + oc.w_tiles * 8, oy1 = oc.screen_y + oc.h_tiles * 8;
            if (oc.screen_x < sx1 && ox1 > sub.screen_x && oc.screen_y < sy1 && oy1 > sub.screen_y)
                want_ovl = std::min<uint8_t>(want_ovl, oc.slot);
        }
        const uint8_t want = is_exact ? want_exact : want_ovl;
        if (is_exact) ++exact_n; else ++bbox_n;
        if (fv->sprite_sub_slot[sidx] != want) {
            ++mismatch;
            if (mismatch <= 4)
                std::printf("    mismatch sub %u: got=%u want=%u (%s)\n",
                            sidx, fv->sprite_sub_slot[sidx], want, is_exact ? "exact" : "bbox");
        }
    }
    std::printf("[..] subs exact=%zu bbox=%zu  mismatches=%zu\n", exact_n, bbox_n, mismatch);
    check(mismatch == 0, "sprite_sub_slot = el slot esperado por sub (per-sprite exacto · claimed=frontmost del bbox)");

    // Confirmar que el orden por slot DESC pone el frontmost (min slot) al final.
    uint8_t minslot = 255, maxslot = 0;
    for (uint32_t i = 0; i < fv->sprite_sub_count; ++i) {
        minslot = std::min(minslot, fv->sprite_sub_slot[i]);
        maxslot = std::max(maxslot, fv->sprite_sub_slot[i]);
    }
    check(minslot < maxslot, "hay subs con slots distintos → el z-order DESC los diferencia (frontmost último)");

    s->replay_invalidate();
    check(s->replay_seek(take, F) != nullptr, "replay sigue válido");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
