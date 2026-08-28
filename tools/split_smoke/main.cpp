// ---------------------------------------------------------------------------
// split_smoke — validación headless de AytherSession::split_recording (Fase C).
//
// Divide una toma .arp real en el frame F y verifica:
//   1. Estructura: head=[0,F) y tail=[F,N) con inputs/historia CSR rebasados.
//   2. DETERMINISMO: el estado de máquina tras replay_seek(tail, k) es
//      byte-idéntico al de replay_seek(original, F+k) — la prueba de que el
//      savestate del tail se capturó PRE-frame F (sin off-by-one).
//
// Uso: split_smoke <core.dll> <rom> <take.arp>|@selftest [frame]
// @selftest graba una toma sintética de 200 frames (inputs variados, incluido
// el multiplexor de 6 botones) en la propia sesión antes de los checks.
// BYOR: el ROM y la toma llegan por argumentos; nada se hardcodea.
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ayther;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "OK " : "FAIL", what);
    if (!ok) ++g_failures;
}

/// FNV-1a del framebuffer del frame producido — lo OBSERVABLE. (El savestate
/// completo no sirve de testigo: incluye contadores tipo "frames desde el
/// último unserialize" que difieren entre dos caminos válidos al mismo frame.)
uint64_t fb_hash(const FrameView* fv) {
    if (!fv || !fv->fb_pixels || !fv->fb_height || !fv->fb_pitch) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(fv->fb_pixels);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t y = 0; y < fv->fb_height; ++y)
        for (uint32_t x = 0; x < fv->fb_pitch; ++x) {
            h ^= p[(size_t)y * fv->fb_pitch + x];
            h *= 1099511628211ull;
        }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "uso: split_smoke <core> <rom> <take.arp> [frame]\n");
        return 2;
    }

    AytherSession::Config cfg;
    cfg.core_path        = argv[1];
    cfg.rom_path         = argv[2];
    cfg.enable_audio     = false;
    cfg.derive_core_pack = false;
    auto sess = AytherSession::create(cfg);
    if (!sess) {
        std::fprintf(stderr, "sesión: %s\n", sess.error.message.c_str());
        return 2;
    }
    AytherSession& s = **sess;

    AytherRecording rec;
    if (std::strcmp(argv[3], "@selftest") == 0) {
        // Boot + toma sintética: inputs que ejercitan el pad de 6 botones.
        for (int i = 0; i < 120; ++i) { s.set_input(0, 0); s.step(); }
        s.record_start();
        for (int i = 0; i < 700; ++i) {   // >2× kReplayKeyInterval → hornea keyframes
            uint16_t m = 0;
            if ((i / 10) % 2)      m |= 1u << 7;                 // RIGHT
            if ((i / 15) % 3 == 1) m |= 1u << 0;                 // B
            if ((i / 7)  % 4 == 2) m |= (1u << 9) | (1u << 10);  // X + L
            if (i % 60 == 30)      m |= 1u << 3;                 // START
            s.set_input(0, m);
            s.step();
        }
        s.record_stop();
        rec = s.take_recording();
    } else {
        auto rec_opt = AytherRecording::load(argv[3]);
        if (!rec_opt) {
            std::fprintf(stderr, "no se pudo cargar %s\n", argv[3]);
            return 2;
        }
        rec = std::move(*rec_opt);
    }
    const uint32_t n = rec.frame_count();
    if (n < 4) { std::fprintf(stderr, "toma demasiado corta (%u frames)\n", n); return 2; }
    const uint32_t f = argc > 4 ? (uint32_t)std::atoi(argv[4]) : n / 2;
    std::printf("toma: %u frames, split en F=%u\n", n, f);

    // ---- Split + estructura -------------------------------------------------
    AytherRecording head, tail;
    check(s.split_recording(rec, f, head, tail), "split_recording devuelve true");
    check(head.frame_count() == f,     "head.frame_count == F");
    check(tail.frame_count() == n - f, "tail.frame_count == N-F");
    check(!head.empty() && !tail.empty(), "ninguna mitad queda vacía");
    check(std::memcmp(head.inputs.data(), rec.inputs.data(),
                      f * sizeof(uint16_t)) == 0, "head.inputs == rec.inputs[0,F)");
    check(std::memcmp(tail.inputs.data(), rec.inputs.data() + f,
                      (n - f) * sizeof(uint16_t)) == 0, "tail.inputs == rec.inputs[F,N)");

    if (rec.hash_offsets.size() == n + 1) {
        check(head.hash_offsets.size() == f + 1,     "head CSR completo");
        check(tail.hash_offsets.size() == n - f + 1, "tail CSR completo");
        bool csr_ok = true;
        for (uint32_t k = 0; k < n - f && csr_ok; k += 7) {
            for (uint32_t i = rec.hash_offsets[f + k];
                 i < rec.hash_offsets[f + k + 1]; ++i)
                if (!tail.present(k, rec.sprite_hashes[i])) { csr_ok = false; break; }
        }
        check(csr_ok, "historia CSR del tail == original re-basada");
    } else {
        std::printf("  (toma sin historia CSR — checks v3 omitidos)\n");
    }

    // Guards del engine.
    AytherRecording h2, t2;
    check(!s.split_recording(rec, 0, h2, t2), "split en F=0 rechazado");
    check(!s.split_recording(rec, n, h2, t2), "split en F=N rechazado");

    // ---- Estabilidad de base: el MISMO seek dos veces debe dar lo mismo -----
    {
        const uint64_t a1 = fb_hash(s.replay_seek(rec, f));
        const uint64_t a2 = fb_hash(s.replay_seek(rec, f));
        check(a1 != 0 && a1 == a2, "seek(rec,F) repetido es estable");
        const uint64_t b1 = fb_hash(s.replay_seek(tail, 0));
        const uint64_t b2 = fb_hash(s.replay_seek(tail, 0));
        check(b1 != 0 && b1 == b2, "seek(tail,0) repetido es estable");
        std::printf("  [diag] rec@F=%016llx tail@0=%016llx\n",
                    (unsigned long long)a1, (unsigned long long)b1);
    }

    // ---- Diagnóstico de fase: ¿tail@0 está corrido N frames? ----------------
    {
        const uint64_t t0 = fb_hash(s.replay_seek(tail, 0));
        for (int d = -2; d <= 2; ++d) {
            if ((int)f + d < 0) continue;
            const uint64_t o = fb_hash(s.replay_seek(rec, f + d));
            std::printf("  [diag] tail@0 %s original@F%+d\n",
                        t0 == o ? "==" : "!=", d);
        }
    }

    // ---- Determinismo observable: tail vs original, head vs original --------
    const uint32_t tn = n - f;
    const uint32_t ks[3] = { 0, tn / 2, tn - 1 };
    for (uint32_t k : ks) {
        const uint64_t a = fb_hash(s.replay_seek(rec, f + k));
        const uint64_t b = fb_hash(s.replay_seek(tail, k));
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "framebuffer tail@%u == original@%u (%016llx)", k, f + k,
                      (unsigned long long)a);
        check(a != 0 && a == b, msg);
    }
    {
        const uint32_t j = f - 1;
        const uint64_t a = fb_hash(s.replay_seek(rec, j));
        const uint64_t b = fb_hash(s.replay_seek(head, j));
        check(a != 0 && a == b, "framebuffer head@F-1 == original@F-1");
    }

    // ---- crop_recording (corte destructivo de Recortar) ---------------------
    // Mismo contrato de savestate que el split: cut@k debe ser byte-idéntico
    // (framebuffer) a original@A+k para un rango interior [A, B).
    {
        const uint32_t ca = f / 2, cb = f + tn / 2;
        AytherRecording cut;
        check(s.crop_recording(rec, ca, cb, cut), "crop_recording devuelve true");
        check(cut.frame_count() == cb - ca, "cut.frame_count == B-A");
        check(std::memcmp(cut.inputs.data(), rec.inputs.data() + ca,
                          (cb - ca) * sizeof(uint16_t)) == 0,
              "cut.inputs == rec.inputs[A,B)");
        check(cut.trim_in == 0 && cut.trim_out == cut.frame_count(),
              "cut queda sin marcas de recorte");
        const uint32_t cn = cb - ca;
        const uint32_t cks[3] = { 0, cn / 2, cn - 1 };
        for (uint32_t k : cks) {
            const uint64_t x = fb_hash(s.replay_seek(rec, ca + k));
            const uint64_t y = fb_hash(s.replay_seek(cut, k));
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "framebuffer cut@%u == original@%u (%016llx)", k, ca + k,
                          (unsigned long long)x);
            check(x != 0 && x == y, msg);
        }
        // begin==0 (solo se recorta la cola): sin re-sim, initial_state directo.
        AytherRecording cut0;
        check(s.crop_recording(rec, 0, cb, cut0), "crop begin=0 devuelve true");
        check(fb_hash(s.replay_seek(cut0, 0)) == fb_hash(s.replay_seek(rec, 0)),
              "framebuffer cut0@0 == original@0");
        // Guards del engine.
        AytherRecording bad;
        check(!s.crop_recording(rec, cb, cb, bad),    "crop rango vacío rechazado");
        check(!s.crop_recording(rec, 0, n + 1, bad),  "crop end>N rechazado");
    }

    // ---- R7d: caminos rápidos de replay_seek (fast +1 + reuso de keyframes) -
    // El seek incremental (cursor vivo + keyframes cacheados) NO debe alterar lo
    // observable. Referencia: el seek "frío" frame a frame (replay_reset fuerza el
    // camino general unserialize+bare+produce que ya validamos arriba).
    {
        const uint32_t T = n - 1;
        std::vector<uint64_t> ref(T + 1);
        for (uint32_t k = 0; k <= T; ++k) { s.replay_reset(); ref[k] = fb_hash(s.replay_seek(rec, k)); }

        // 1) FAST +1: cadena pura de produce desde 0 (el camino del playback).
        s.replay_reset();
        bool fast_ok = (fb_hash(s.replay_seek(rec, 0)) == ref[0]);
        for (uint32_t k = 1; k <= T && fast_ok; ++k)
            fast_ok = (fb_hash(s.replay_seek(rec, k)) == ref[k]);   // pos==k-1 → fast +1
        check(fast_ok, "replay_seek fast +1 (playback) == seek frio en cada frame");

        // 2) REUSO DE KEYFRAMES: la cadena anterior pobló keyframes; los saltos
        // hacia atrás (paso -7 → nunca +1) arrancan de un keyframe, SIN reset.
        bool key_ok = true;
        for (int k = (int)T; k >= 0 && key_ok; k -= 7)
            key_ok = (fb_hash(s.replay_seek(rec, (uint32_t)k)) == ref[(uint32_t)k]);
        check(key_ok, "replay_seek reuso de keyframes (saltos) == seek frio");

        // 3) SEEK EN CHUNKS (R7e): bombeado a completar con budget chico == frio.
        s.replay_reset();
        AytherSession::SeekStep st{};
        int guard = 0;
        do { st = s.replay_seek_chunk(rec, T, 37); } while (!st.done && ++guard < 1000000);
        check(st.done && st.view && fb_hash(st.view) == ref[T],
              "replay_seek_chunk (troceado) == seek frio");

        // 4) KEYFRAMES HORNEADOS (R7e): la toma sintética los trae; un seek frío
        // cerca del último arranca de él (costo bajo) y rinde el frame correcto.
        check(!rec.keyframes.empty(), "la toma sintetica horneo keyframes");
        if (!rec.keyframes.empty()) {
            const uint32_t kf  = rec.keyframes.back().frame;
            const uint32_t tgt = std::min(kf + 25u, T);
            s.replay_reset();
            const uint32_t cost = s.replay_seek_cost(rec, tgt);
            s.replay_reset();
            check(cost <= 300u && fb_hash(s.replay_seek(rec, tgt)) == ref[tgt],
                  "seek frio desde keyframe horneado: barato y correcto");
        }

        // 5) MIGRACIÓN (R7e): una toma SIN keyframes (estilo v3) → replay_bake_step
        // la hornea troceada; tras eso un seek frío lejano es barato y correcto.
        AytherRecording old_rec = rec;
        old_rec.keyframes.clear();                 // simula una toma vieja sin keyframes
        s.replay_reset();
        AytherSession::SeekStep bs{};
        int bguard = 0;
        do { bs = s.replay_bake_step(old_rec, 113); } while (!bs.done && ++bguard < 1000000);
        check(!old_rec.keyframes.empty(), "migracion: replay_bake_step horneo keyframes");
        s.replay_reset();
        const uint32_t mtgt = T - 5;
        const uint32_t mcost = s.replay_seek_cost(old_rec, mtgt);
        s.replay_reset();
        check(mcost <= 300u && fb_hash(s.replay_seek(old_rec, mtgt)) == ref[mtgt],
              "migracion: seek frio post-bake barato y correcto");
    }

    std::printf(g_failures ? "\nsplit_smoke: %d FALLAS\n"
                           : "\nsplit_smoke: PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
