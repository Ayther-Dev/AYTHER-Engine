// ---------------------------------------------------------------------------
// element_group_hide_smoke (2026-07-31) — OCULTAR UN MIEMBRO DESACTIVA EL ASSET
// DEL GRUPO.
//
// Un asset HD reemplaza a un GRUPO de occurrences (la pose / el objeto), no a
// una sola: el ancla dibuja el HD y los demás miembros quedan `claimed` (su
// original no se dibuja porque el HD los tapa). Con ese modelo el ojo del
// Inspector no se notaba — ocultar un miembro no cambiaba nada (el HD lo seguía
// tapando) y ocultar el ancla tampoco (la lane plana de sprites lo re-dibujaba).
//
// Contrato que fija este oráculo (el mismo del panel Sprites de Posar): si
// CUALQUIER elemento del grupo está oculto, el asset se DESACTIVA (claimed=0 en
// todo el grupo) y los miembros que siguen visibles vuelven a dibujar su
// original. El ancla conserva su `sub` para que el renderer sepa que ese sub
// tiene ancla y no lo re-dibuje plano encima.
//
//   1. Baseline: la pose reclama N>=2 elementos — 1 ancla (sub>=0) + miembros.
//   2. Ocultar un MIEMBRO → todo el grupo queda claimed=0; sólo el ocultado
//      queda hidden=1 (los otros dibujan su original).
//   3. Ocultar el ANCLA → idem (el HD se apaga, los miembros reaparecen).
//   4. Limpiar → vuelve al baseline EXACTO (claimed restaurado, sin marcas).
//   5. Sin contagio: un grupo de OTRO sub conserva su claimed.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target element_group_hide_smoke
//   Args:  <poses.toml> <rec.arp> [f0] [f1]   (default barrido 0..600)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;
using SceneElement = ayther::AytherSession::SceneElement;
using PosePreview  = ayther::AytherSession::PosePreview;
using HE           = ayther::AytherSession::HiddenElement;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// Parser mínimo de poses.toml (mismo subset que hd_audio_probe).
struct PoseDef {
    std::string asset;
    std::vector<uint64_t> hashes;
    std::vector<std::pair<int16_t, int16_t>> rel, dims;
};
static std::vector<PoseDef> load_poses(const std::string& path) {
    std::vector<PoseDef> out;
    std::ifstream f(path);
    std::string line;
    PoseDef cur;
    auto flush = [&]() { if (!cur.hashes.empty()) out.push_back(cur); cur = PoseDef{}; };
    while (std::getline(f, line)) {
        if (line.find("[[pose]]") != std::string::npos) { flush(); continue; }
        auto starts = [&](const char* k) { return line.rfind(k, 0) == 0; };
        if (starts("default_asset")) cur.asset = toml_quoted(line);
        else if (starts("hashes")) {
            const std::string v = toml_quoted(line);
            for (size_t p = 0; p < v.size(); ) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                cur.hashes.push_back(std::strtoull(v.substr(p, e - p).c_str(), nullptr, 16));
                p = e + 1;
            }
        } else if (starts("rel") || starts("dims")) {
            auto& dst = starts("rel") ? cur.rel : cur.dims;
            const std::string v = toml_quoted(line);
            for (size_t p = 0; p < v.size(); ) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                const std::string it = v.substr(p, e - p);
                const size_t c = it.find(',');
                if (c != std::string::npos)
                    dst.push_back({ (int16_t)std::atoi(it.substr(0, c).c_str()),
                                    (int16_t)std::atoi(it.substr(c + 1).c_str()) });
                p = e + 1;
            }
        }
    }
    flush();
    return out;
}

// Un GRUPO del inventario: el ancla (sub>=0) + todos los elementos claimed que
// el buscador atribuye al mismo sub. Acá se reconstruye desde el inventario con
// el mismo criterio observable: los claimed de la capa 3 en el frame.
struct Group {
    int32_t  sub = -1;
    uint64_t anchor_hash = 0;
    std::vector<uint64_t> member_hashes;   // sin el ancla
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "uso: element_group_hide_smoke <poses.toml> <rec.arp> [f0] [f1]\n");
        return 2;
    }
    const std::string poses_path = argv[1], rec_path = argv[2];
    const uint32_t f0 = argc > 3 ? (uint32_t)std::strtoul(argv[3], nullptr, 10) : 0u;
    const uint32_t f1 = argc > 4 ? (uint32_t)std::strtoul(argv[4], nullptr, 10) : 600u;

    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, rom, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos && core.empty())
                core = toml_quoted(line);
    }
    core = resolve(core, root);
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;
    if (rom.empty()) { std::fprintf(stderr, "[FAIL] falta AYTHER_PROBE_ROM\n"); return 2; }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    std::vector<PosePreview> pvs;
    for (const auto& p : load_poses(poses_path)) {
        if (p.asset.empty() || p.rel.size() != p.hashes.size()) continue;
        PosePreview pv;
        pv.hashes = p.hashes;
        pv.asset  = p.asset;
        pv.hd     = true;
        for (auto& rl : p.rel) { pv.rel_x.push_back(rl.first); pv.rel_y.push_back(rl.second); }
        if (p.dims.size() == p.hashes.size())
            for (auto& d : p.dims) { pv.dim_w.push_back(d.first); pv.dim_h.push_back(d.second); }
        pvs.push_back(std::move(pv));
    }
    std::printf("=== element_group_hide_smoke ===\nposes con HD: %zu\n\n", pvs.size());
    if (pvs.empty()) { check(false, "hay poses con asset en el catálogo"); return 1; }

    auto produce = [&](uint32_t f) -> const FrameView* {
        s->replay_invalidate();
        s->replay_seek(*rec, f);
        s->replay_invalidate();
        return s->replay_seek(*rec, f);
    };
    auto inventory = [&]() {
        std::vector<SceneElement> inv;
        s->scene_inventory(inv);
        return inv;
    };

    // ---- buscar un frame con una pose que reclame >=2 elementos ------------
    s->set_pose_preview(pvs);
    s->set_hidden_elements(nullptr, 0);
    uint32_t F = 0;
    Group g;
    std::vector<SceneElement> base;
    for (uint32_t f = f0; f <= f1 && g.sub < 0; ++f) {
        if (!produce(f)) continue;
        std::vector<SceneElement> inv = inventory();
        int32_t anchor_sub = -1;
        uint64_t anchor_hash = 0;
        size_t claimed_n = 0;
        for (const SceneElement& e : inv) {
            if (e.layer != 3 || !e.claimed) continue;
            ++claimed_n;
            if (e.sub >= 0 && anchor_sub < 0) { anchor_sub = e.sub; anchor_hash = e.hash; }
        }
        if (anchor_sub < 0 || claimed_n < 2) continue;
        // Grupo = los claimed de la capa 3 (en este frame la pose es la única
        // que reclama; el check 5 verifica el no-contagio cuando hay más).
        g.sub = anchor_sub;
        g.anchor_hash = anchor_hash;
        for (const SceneElement& e : inv)
            if (e.layer == 3 && e.claimed && e.hash != anchor_hash)
                g.member_hashes.push_back(e.hash);
        F = f;
        base = std::move(inv);
    }
    if (g.sub < 0) {
        check(false, "hay un frame con una pose que reclama >=2 elementos");
        std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
        return 1;
    }
    std::printf("frame %u · sub %d · ancla 0x%016llx · %zu miembros\n\n",
                F, g.sub, (unsigned long long)g.anchor_hash, g.member_hashes.size());

    // Recuento del grupo en un inventario: (claimed, hidden) de sus hashes.
    auto tally = [&](const std::vector<SceneElement>& inv,
                     size_t& claimed, size_t& hidden, size_t& total) {
        claimed = hidden = total = 0;
        for (const SceneElement& e : inv) {
            if (e.layer != 3) continue;
            bool mine = e.hash == g.anchor_hash;
            for (uint64_t h : g.member_hashes) mine |= (e.hash == h);
            if (!mine) continue;
            ++total;
            if (e.claimed) ++claimed;
            if (e.hidden)  ++hidden;
        }
    };

    // ---- 1. baseline: todo el grupo reclamado, nada oculto -----------------
    {
        size_t cl = 0, hd = 0, tot = 0;
        tally(base, cl, hd, tot);
        check(tot >= 2 && cl == tot, "baseline: todo el grupo está claimed (el HD lo reemplaza)");
        check(hd == 0, "baseline: nada oculto");
    }

    // ---- 2. ocultar un MIEMBRO desactiva el asset del grupo -----------------
    {
        HE h[1] = { { g.member_hashes.front(), 3 } };
        s->set_hidden_elements(h, 1);
        produce(F);
        const std::vector<SceneElement> inv = inventory();
        size_t cl = 0, hd = 0, tot = 0;
        tally(inv, cl, hd, tot);
        check(cl == 0, "ocultar un MIEMBRO desactiva el asset (claimed=0 en todo el grupo)");
        check(hd >= 1, "el miembro ocultado queda hidden=1");
        check(hd < tot, "los otros miembros siguen visibles → dibujan su original");
        // El ancla conserva su sub: el renderer sabe que el sub tiene ancla y
        // no lo re-dibuja plano en la lane de sprites.
        bool anchor_keeps_sub = false;
        for (const SceneElement& e : inv)
            if (e.layer == 3 && e.hash == g.anchor_hash && e.sub == g.sub)
                anchor_keeps_sub = true;
        check(anchor_keeps_sub, "el ancla conserva su `sub` (la lane plana no lo re-dibuja)");
    }

    // ---- 3. ocultar el ANCLA: mismo contrato -------------------------------
    {
        HE h[1] = { { g.anchor_hash, 3 } };
        s->set_hidden_elements(h, 1);
        produce(F);
        const std::vector<SceneElement> inv = inventory();
        size_t cl = 0, hd = 0, tot = 0;
        tally(inv, cl, hd, tot);
        check(cl == 0, "ocultar el ANCLA desactiva el asset (claimed=0 en todo el grupo)");
        check(hd >= 1 && hd < tot, "el ancla queda oculta y los miembros reaparecen");
    }

    // ---- 4. limpiar restaura el baseline EXACTO ----------------------------
    {
        s->set_hidden_elements(nullptr, 0);
        produce(F);
        const std::vector<SceneElement> inv = inventory();
        size_t cl = 0, hd = 0, tot = 0;
        tally(inv, cl, hd, tot);
        check(hd == 0 && cl == tot, "limpiar restaura el grupo (claimed de vuelta, sin marcas)");
        bool same = inv.size() == base.size();
        for (size_t i = 0; same && i < inv.size(); ++i)
            same = inv[i].hash == base[i].hash && inv[i].claimed == base[i].claimed &&
                   inv[i].hidden == base[i].hidden && inv[i].sub == base[i].sub;
        check(same, "el inventario vuelve IDÉNTICO al baseline");
    }

    // ---- 5. sin contagio entre grupos --------------------------------------
    // Un frame con DOS subs anclados: ocultar un elemento de uno no debe
    // desactivar el asset del otro (dos poses solapadas — Tyris bajo el dragón).
    {
        uint32_t F2 = 0;
        int32_t subA = -1, subB = -1;
        uint64_t hashA = 0, hashB = 0;
        for (uint32_t f = f0; f <= f1 && subB < 0; ++f) {
            if (!produce(f)) continue;
            const std::vector<SceneElement> inv = inventory();
            subA = subB = -1; hashA = hashB = 0;
            for (const SceneElement& e : inv) {
                if (e.layer != 3 || e.sub < 0 || !e.claimed) continue;
                if (subA < 0)               { subA = e.sub; hashA = e.hash; }
                else if (e.sub != subA && subB < 0) { subB = e.sub; hashB = e.hash; }
            }
            if (subB >= 0) F2 = f;
        }
        if (subB < 0) {
            std::printf("  [skip] no hay frame con dos poses ancladas en %u..%u\n", f0, f1);
        } else {
            produce(F2);
            HE h[1] = { { hashA, 3 } };
            s->set_hidden_elements(h, 1);
            produce(F2);
            const std::vector<SceneElement> inv = inventory();
            bool other_alive = false;
            for (const SceneElement& e : inv)
                if (e.layer == 3 && e.hash == hashB && e.sub == subB && e.claimed)
                    other_alive = true;
            std::printf("  (f%u · sub A=%d hash 0x%016llx · sub B=%d hash 0x%016llx)\n",
                        F2, subA, (unsigned long long)hashA,
                        subB, (unsigned long long)hashB);
            check(other_alive, "ocultar en un grupo NO desactiva el asset del otro");
            s->set_hidden_elements(nullptr, 0);
        }
    }

    s->set_pose_preview({});
    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
