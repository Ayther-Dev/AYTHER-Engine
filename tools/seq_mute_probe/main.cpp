// ---------------------------------------------------------------------------
// seq_mute_probe — valida el mute SELECTIVO por firma de las Secuencias
// (AudioSeqSub.signatures, 2026-07-23): dentro de la ventana disparada, el
// motor debe mutear SOLO los eventos miembro (por su propio span), no el canal
// completo — antes channel_mask silenciaba todo el canal durante la ventana y
// se llevaba puesta la música que lo compartía.
//
// Método: sesión + toma real, analyze_audio_events, se arma una AudioSeqSub
// sintética con los eventos de una firma real (miembros) y ventana holgada.
// Se recorre el replay dos veces (signatures llenas vs vacías = fallback) y se
// compara audio_mute_mask por frame contra el cálculo esperado.
//
// La expectativa modela la COLA DE RELEASE de #280 (#330): el key-off no calla
// al FM — mutear hasta end+kTail frames es comportamiento CORRECTO, y la cola
// cede ante un evento AJENO activo en el mismo canal. Es el mismo modelo de
// Impl::event_mute_with_tail que instrument_mute_probe ya replica; la versión
// anterior de este probe exigía «evento miembro ACTIVO» y quedó vieja al
// mergear #280 (fallaba con 1 frame «de más» que era exactamente la cola).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target seq_mute_probe
//   Args:  <core.dll> <rom> <recording.arp>
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using ayther::AytherSession;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "uso: %s <core.dll> <rom> <recording.arp>\n", argv[0]);
        return 2;
    }
    AytherSession::Config cfg;
    cfg.core_path = argv[1];
    cfg.rom_path  = argv[2];
    cfg.enable_audio = false;   // sin device: el mute igual se computa (mask)
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create\n"); return 1; }
    std::unique_ptr<AytherSession>& s = *r;

    // Mide la máscara SUSTRACTIVA, que desde #326 no es el camino por defecto
    // (con el router puesto `audio_mute_mask` es 0x3FF constante). El camino
    // viejo sigue detrás de AYTHER_VOICE_ROUTER=0 y hay que pedirlo acá.
    s->set_voice_router(false);

    auto rec_opt = ayther::AytherRecording::load(argv[3]);
    if (!rec_opt) { std::fprintf(stderr, "[FAIL] rec\n"); return 1; }
    const ayther::AytherRecording& rec = *rec_opt;

    const uint32_t nev = s->analyze_audio_events(rec);
    const AytherAudioEvent* evs = s->audio_events();
    if (nev < 4) { std::fprintf(stderr, "[FAIL] pocos eventos (%u)\n", nev); return 1; }

    // Miembro sintético: la firma del primer evento FM que se repita >=2 veces.
    uint64_t member_sig = 0; uint16_t member_bit = 0;
    for (uint32_t i = 0; i < nev && !member_sig; ++i) {
        if (evs[i].chip != 0) continue;
        int reps = 0;
        for (uint32_t j = 0; j < nev; ++j)
            if (evs[j].signature == evs[i].signature) ++reps;
        if (reps >= 2) {
            member_sig = evs[i].signature;
            member_bit = uint16_t(1u << evs[i].channel);
        }
    }
    if (!member_sig) { std::fprintf(stderr, "[FAIL] sin firma repetida\n"); return 1; }

    // Ventana: 1ª ocurrencia como trigger, duración holgada (span + 120 frames)
    // para que la ventana EXCEDA el span del evento — ahí es donde el mask por
    // canal muteaba de más y el selectivo debe quedar en 0.
    uint32_t trig_start = 0, trig_end = 0;
    for (uint32_t i = 0; i < nev; ++i)
        if (evs[i].signature == member_sig) { trig_start = evs[i].start_frame;
                                              trig_end   = evs[i].end_frame; break; }
    AytherSession::AudioSeqSub sub;
    sub.trigger_signature = member_sig;
    sub.duration_frames   = (trig_end - trig_start + 1) + 120;
    sub.channel_mask      = member_bit;
    sub.key               = 1;
    sub.asset             = "probe.wav";   // no vacío (no suena: enable_audio=false)
    sub.signatures        = { member_sig };

    s->set_audio_substitution_preview(true);

    auto run = [&](bool selective) {
        AytherSession::AudioSeqSub v = sub;
        if (!selective) v.signatures.clear();   // fallback viejo: mask por canal
        s->set_audio_sequence_subs({ v });
        s->replay_invalidate();
        uint32_t muted = 0, over = 0;
        const uint32_t f1 = std::min(rec.frame_count(), trig_start + sub.duration_frames + 30);
        for (uint32_t f = trig_start; f < f1; ++f) {
            const ayther::FrameView* fv = s->replay_seek(rec, f);
            if (!fv) continue;
            if (!(fv->audio_mute_mask & member_bit)) continue;
            ++muted;
            // ¿Muteado de MÁS? Legítimo = ventana propia de un miembro, o su
            // cola de release (end, end+kTail] si ningún evento AJENO activo
            // defiende el canal en este frame (= event_mute_with_tail, #280).
            constexpr uint32_t kTail = 15;   // = Impl::kMuteTailFrames
            bool own = false, tail = false, foreign = false;
            for (uint32_t i = 0; i < nev; ++i) {
                const AytherAudioEvent& e = evs[i];
                const uint16_t bit = e.chip == 0
                                         ? uint16_t(1u << e.channel)
                                         : uint16_t(1u << (6 + e.channel));
                if (bit != member_bit) continue;
                if (e.signature == member_sig) {
                    if (f >= e.start_frame && f <= e.end_frame)           own  = true;
                    else if (f > e.end_frame && f <= e.end_frame + kTail) tail = true;
                } else if (f >= e.start_frame && f <= e.end_frame) {
                    foreign = true;
                }
            }
            if (!(own || (tail && !foreign))) ++over;
        }
        return std::pair<uint32_t, uint32_t>(muted, over);
    };

    const auto sel = run(true);
    const auto old = run(false);
    std::printf("firma miembro %016llx  canal bit 0x%03x  ventana %u frames\n",
                (unsigned long long)member_sig, member_bit, sub.duration_frames);
    std::printf("selectivo: %u frames muteados, %u de MAS (fuera de span+cola de release)\n",
                sel.first, sel.second);
    std::printf("fallback : %u frames muteados, %u de mas\n", old.first, old.second);

    // Asertos: el selectivo nunca mutea sin evento miembro activo; el fallback
    // (ventana > span) sí muteaba de más — y el selectivo mutea MENOS que él.
    const bool ok = sel.second == 0 && sel.first > 0 &&
                    old.second > 0 && sel.first < old.first;
    std::printf("\n%s\n", ok ? "[OK] mute selectivo por firma: solo los eventos de la Secuencia"
                             : "[FAIL] ver arriba");
    return ok ? 0 : 5;
}
