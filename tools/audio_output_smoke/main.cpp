// ---------------------------------------------------------------------------
// audio_output_smoke — ¿SUENA? El oráculo del camino que el usuario oye.
//
// POR QUÉ EXISTE. El 2026-08-13 se descubrió que el Lab llevaba DOS DÍAS mudo
// en cartucho y que el detector de audio devolvía cero eventos: el fork había
// cambiado el significado de `ayther_audio_write_v1.addr` —de índice de puerto
// del bus a registro ya latcheado— y los tres consumidores seguían con el
// `addr & 3` viejo. En esos dos días `ctest` estuvo en 45/45.
//
// Pudo pasar porque el repo tiene DOS MUNDOS que no se tocan: 45 tests que no
// necesitan ROM (y por eso no ven el core de verdad), y 63 herramientas que sí
// la necesitan y que por eso se corren A MANO, cuando alguien se acuerda. El
// bug vivió justo en el medio.
//
// Este smoke cierra esa brecha con una ROM SINTÉTICA: 68k escrito acá, que
// programa el YM2612 y el PSG y hace key-on. Sin ROM comercial no hay licencia
// que discutir, el estímulo es conocido —sabemos qué tiene que sonar— y entra
// al ctest de todos los días.
//
// LO QUE MIDE. No el buffer del core, que es lo que miran los otros 63: la
// MEZCLA QUE SE LE ENCOLA AL DEVICE, que es por donde el usuario oye. Y la mide
// con el ROUTER DE VOCES PUESTO, que es el default y era justo el camino roto.
//
// EL CONTROL. Dos ROMs: una que hace key-on y otra IDÉNTICA que no lo hace. La
// primera tiene que sonar y la segunda tiene que dar silencio. Sin el par, un
// «suena» puede ser ruido de arranque y un «no suena» puede ser un medidor roto
// — es la misma lección del control A-vs-A de #409.
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include "../common/synth_rom.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// ---------------------------------------------------------------------------
// La ROM. El ensamblador 68k y el header de Mega Drive viven en
// tools/common/synth_rom.h desde que el oráculo de render (#415) necesitó los
// mismos: acá queda sólo lo que es de AUDIO, que es el programa.
// ---------------------------------------------------------------------------
using Rom = ayther::synth::Rom;

/// El programa: toma el bus del Z80 (sin eso el 68k no llega a los chips),
/// programa el canal 1 del FM con un patch que suena solo —algoritmo 7, los
/// cuatro operadores son portadores, ataque instantáneo y sin decay— y abre el
/// PSG. `key_on` decide si además dispara la nota: con false la ROM es idéntica
/// y tiene que dar SILENCIO, que es el control de este oráculo.
void program(Rom& r, bool key_on) {
    r.take_z80_bus();                    // pedirlo, sacar al Z80 del reset y
                                         // ESPERAR a que lo conceda de verdad

    r.fm(0x22, 0x00);                    // LFO apagado
    r.fm(0x27, 0x00);                    // sin timers, canal 3 normal
    r.fm(0x28, 0x00);                    // todo en key-off
    r.fm(0x2B, 0x00);                    // DAC apagado
    for (uint8_t op = 0; op < 4; ++op) {
        const uint8_t o = uint8_t(op * 4);   // canal 0 del banco 0
        r.fm(uint8_t(0x30 + o), 0x01);   // DT=0 MUL=1
        r.fm(uint8_t(0x40 + o), 0x00);   // TL=0 → nivel máximo
        r.fm(uint8_t(0x50 + o), 0x1F);   // AR=31 → ataque instantáneo
        r.fm(uint8_t(0x60 + o), 0x00);   // D1R=0 → no decae
        r.fm(uint8_t(0x70 + o), 0x00);   // D2R=0
        r.fm(uint8_t(0x80 + o), 0x0F);   // D1L=0 RR=15
        r.fm(uint8_t(0x90 + o), 0x00);   // SSG-EG apagado
    }
    r.fm(0xB0, 0x07);                    // FB=0 ALG=7 (los cuatro, portadores)
    r.fm(0xB4, 0xC0);                    // sale por izquierda y derecha
    r.fm(0xA4, 0x22);                    // block 4 + fnum alto
    r.fm(0xA0, 0x69);                    // fnum bajo  (≈ 440 Hz)
    if (key_on) r.fm(0x28, 0xF0);        // ← la nota

    r.psg(0x80 | 0x0E);                  // canal 0: latch de tono, nibble bajo
    r.psg(0x0F);                         // resto del tono
    r.psg(0x90 | (key_on ? 0x00 : 0x0F));// atenuación: 0 = a pleno · F = mudo

    if (!key_on) { r.spin(); return; }   // la ROM de control termina muda acá

    // EL DAC, que es el tercer camino de audio y el que más fácil se pierde de
    // vista: en Golden Axe no aparece —su attract no lo usa— y por eso el
    // registro 0x2A estuvo en cero todo un día sin que se supiera si era el
    // juego o el fork. Acá se ejercita a propósito.
    //
    // El detector NO lo segmenta por key-on —este chip no tiene— sino por
    // SILENCIO: un frame cuenta como «con señal» si sus muestras VARÍAN. Por eso
    // el bucle alterna 0x00 y 0xFF en vez de escribir un valor fijo, que el
    // detector leería —con razón— como silencio.
    r.fm(0x2B, 0x80);                    // DAC enable: el canal 6 pasa a ser PCM
    r.write_byte_to(0xA04000, 0x2A);     // latch de 0x2A UNA vez: desde acá, cada
                                         // escritura al puerto de datos es una muestra
    const uint32_t loop = r.here();
    r.write_byte_to(0xA04001, 0x00);
    r.write_byte_to(0xA04001, 0xFF);
    r.loop_to(loop);
}

/// RMS del WAV que deja el tee del player (S16 estéreo). Lee el PCM crudo desde
/// el byte 44: los tamaños del header los parchea el shutdown y acá el archivo
/// puede quedar sin parchear si algo falla antes. -1 si no hay nada que medir.
double wav_rms(const std::string& path) {
    std::FILE* f = ayther::file_open(path.c_str(), "rb");
    if (!f) return -1.0;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    if (sz <= 44) { std::fclose(f); return -1.0; }
    std::fseek(f, 44, SEEK_SET);
    std::vector<int16_t> pcm(size_t(sz - 44) / 2);
    const size_t got = std::fread(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    if (!got) return -1.0;
    double acc = 0.0;
    for (size_t i = 0; i < got; ++i) acc += double(pcm[i]) * double(pcm[i]);
    return std::sqrt(acc / double(got));
}

/// Un WAV S16 estéreo 44100 con una onda cuadrada de amplitud `amp` (0 = un
/// asset SILENCIOSO, que es un control y no un caso raro: sirve para separar
/// «el original calló» de «el HD tapó al original»).
bool write_wav(const std::string& path, int16_t amp, uint32_t frames) {
    std::FILE* f = ayther::file_open(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t data = frames * 4, riff = 36 + data;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(riff); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    for (uint32_t i = 0; i < frames; ++i) {          // ~440 Hz
        const int16_t v = (i / 50) % 2 ? amp : int16_t(-amp);
        u16(uint16_t(v)); u16(uint16_t(v));
    }
    std::fclose(f);
    return true;
}

void setenv_(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

struct Run {
    double   rms    = -1.0;  ///< nivel de lo que se le encoló al device
    uint32_t active = 0;     ///< canales que el DETECTOR ve sonando al final
    uint32_t active_fm = 0;  ///< …de ellos, del FM. Por separado a propósito: el
                             ///< PSG viaja por otro camino y sobrevive a los
                             ///< fallos del FM, así que un «hay alguno» tapa
                             ///< justo el chip que se rompe (medido: con el bug
                             ///< del 2026-08-13 el detector seguía viendo 1).
    // El camino de AUTORÍA, que es OTRO: el usuario no asigna sobre lo que suena
    // ahora, graba una toma y la analiza. Ese análisis es el que devolvía CERO
    // eventos esta mañana, y comparte causa con el mudo pero no camino — el de
    // #409 se rompió solo, sin tocar el audio en vivo.
    uint32_t events     = 0;   ///< eventos que salen de analizar la toma
    uint32_t with_sig   = 0;   ///< …con firma (lo que se asigna)
    uint32_t fm_events  = 0;
    uint32_t psg_events = 0;
    uint32_t dac_events = 0;   ///< FM6 sin altura = el DAC
    // PACING (#146). «Suena» y «suena BIEN» son cosas distintas: el audio puede
    // llegar entero y aun así entrecortado. Esa cacería costó diez causas raíz
    // encadenadas y quedó con contadores pero sin nadie que los mire.
    uint64_t starved   = 0;   ///< frames en que el router entregó de menos
    uint64_t flushed   = 0;   ///< frames en que el PCM SÍ salió al device
    uint64_t inaudible = 0;   ///< …y frames en que no salió, por gate
};

/// Corre la ROM y mide las DOS mitades del camino: lo que se oye y lo que el
/// detector entiende. Van juntas a propósito — el bug del 2026-08-13 rompió las
/// dos de un saque (el chip mudo y el detector ciego), y separarlas en dos
/// oráculos habría dejado a cada uno pasando por su lado.
///
/// `router` elige el camino de salida: true = el espejo de voces (el default
/// del Lab), false = el PCM del chip tal como sale del core.
Run measure(const std::string& core, const std::string& rom,
            const std::string& wav, bool router, int frames,
            const char* save_take = nullptr) {
    Run out;
    std::filesystem::remove(wav);
    setenv_("AYTHER_AUDIO_DUMP", wav.c_str());
    ayther::AytherSession::Config c;
    c.core_path = core.c_str();
    c.rom_path  = rom.c_str();
    c.enable_audio = true;
    auto sr = ayther::AytherSession::create(c);
    if (!sr) { setenv_("AYTHER_AUDIO_DUMP", ""); return out; }
    {
        std::unique_ptr<ayther::AytherSession>& s = *sr;
        // Explícito en las dos ramas: qué mide este control no lo decide el
        // AYTHER_VOICE_ROUTER que tenga puesto el que corre los tests.
        s->set_voice_router(router);
        // Se graba DESDE EL FRAME 0 para que la toma incluya el key-on: si
        // empezara después, el detector vería una nota ya abierta y el análisis
        // mediría otra cosa.
        s->record_start();
        for (int f = 0; f < frames; ++f) s->step();
        s->record_stop();
        AytherAudioActive act[16];
        out.active = s->audio_live_active(act, 16);
        for (uint32_t i = 0; i < out.active && i < 16; ++i)
            if (act[i].chip == 0) ++out.active_fm;
        // Pacing: se lee ANTES del análisis, que re-emula la toma en silencio y
        // ensuciaría los contadores de entrega con frames que nunca iban a sonar.
        s->voice_router_stats(nullptr, nullptr, nullptr, &out.starved);
        s->audio_gate_counts(&out.flushed, &out.inaudible, nullptr, nullptr);

        // El análisis re-emula la toma en SILENCIO (replay_quiet), así que no
        // agrega audio al tee y no contamina el RMS de arriba.
        const ayther::AytherRecording rec = s->take_recording();
        if (save_take) rec.save(save_take);   // la reusa la prueba de sustitución
        out.events = s->analyze_audio_events(rec);
        const AytherAudioEvent* evs = s->audio_events();
        for (uint32_t i = 0; i < out.events && evs; ++i) {
            if (evs[i].signature) ++out.with_sig;
            if (evs[i].chip != 0)                                ++out.psg_events;
            else if (evs[i].channel == 5 && evs[i].pitch == 255) ++out.dac_events;
            else                                                 ++out.fm_events;
        }
    }   // el destructor cierra el tee
    setenv_("AYTHER_AUDIO_DUMP", "");
    out.rms = wav_rms(wav);
    return out;
}

/// LA SUSTITUCIÓN, que es lo que el producto promete: reproduce la toma con el
/// preview puesto y devuelve el nivel de lo que sale al device. Con
/// `hd_asset` = nullptr no sustituye nada (la referencia); con un WAV, se lo
/// asigna a la firma del evento de FM y deja que el motor haga su trabajo:
/// callar el original en su ventana y disparar el asset en su lugar.
///
/// Va por REPLAY de una toma guardada y no en vivo, para que el tee capture
/// SÓLO esta pasada — midiendo sobre la sesión que además corrió en vivo, el
/// audio de las dos se suma en el mismo archivo y el número no dice nada.
double substitution_rms(const std::string& core, const std::string& rom,
                        const std::string& rec_path, const char* hd_asset,
                        const std::string& wav, bool router = true,
                        uint64_t* substituted = nullptr) {
    auto rec_opt = ayther::AytherRecording::load(rec_path);
    if (!rec_opt) return -1.0;
    const ayther::AytherRecording& rec = *rec_opt;

    std::filesystem::remove(wav);
    setenv_("AYTHER_AUDIO_DUMP", wav.c_str());
    ayther::AytherSession::Config c;
    c.core_path = core.c_str();
    c.rom_path  = rom.c_str();
    c.enable_audio = true;
    auto sr = ayther::AytherSession::create(c);
    if (!sr) { setenv_("AYTHER_AUDIO_DUMP", ""); return -1.0; }
    {
        std::unique_ptr<ayther::AytherSession>& s = *sr;
        s->set_voice_router(router);
        const uint32_t nev = s->analyze_audio_events(rec);
        const AytherAudioEvent* evs = s->audio_events();
        if (hd_asset) {
            for (uint32_t i = 0; i < nev && evs; ++i)
                if (evs[i].chip == 0 && evs[i].pitch != 255) {   // la nota de FM
                    s->assign_audio_event(evs[i].signature, hd_asset);
                    break;
                }
            s->set_audio_substitution_preview(true);
        }
        s->set_transport_playing(true);
        s->replay_invalidate();
        for (uint32_t f = 0; f < rec.frame_count(); ++f) s->replay_seek(rec, f);
        // Key-ons que NO cayeron en copia: el único observable desde afuera de
        // que la decisión de mute LLEGÓ al router. Con el chip sonando entero
        // —que es lo que el router necesita para que el hasher lo vea— la
        // máscara de mute es 0 siempre y no dice nada (#329).
        if (substituted)
            s->voice_router_stats(nullptr, nullptr, nullptr, nullptr, substituted);
    }
    setenv_("AYTHER_AUDIO_DUMP", "");
    return wav_rms(wav);
}

}  // namespace

int main() {
    std::printf("=== audio_output_smoke — ¿lo que sale por el device SUENA? ===\n");

    const std::string core =
#ifdef AYTHER_SOURCE_DIR
        std::string(AYTHER_SOURCE_DIR) + "/third_party/cores/genesis_plus_gx_libretro_vram.dll";
#else
        "genesis_plus_gx_libretro_vram.dll";
#endif
    if (!std::filesystem::exists(core)) {
        std::printf("[skip] no está el core del fork: %s\n", core.c_str());
        return 77;
    }

    const auto dir = std::filesystem::temp_directory_path();
    const std::string rom_on  = (dir / "ayther_audio_smoke_on.md").string();
    const std::string rom_off = (dir / "ayther_audio_smoke_silent.md").string();
    {
        Rom a("AUDIO OUTPUT SMOKE"); program(a, /*key_on=*/true);
        Rom b("AUDIO OUTPUT SMOKE"); program(b, /*key_on=*/false);
        if (!a.save(rom_on) || !b.save(rom_off)) {
            std::fprintf(stderr, "[FAIL] no pude escribir las ROMs sintéticas\n");
            return 1;
        }
    }
    std::printf("  ROM sintética: %s\n", rom_on.c_str());

    // Device dummy: no hay que oír nada, hay que MEDIR el bloque que se encola.
    setenv_("SDL_AUDIODRIVER", "dummy");
    constexpr int kFrames = 180;   // ~3 s de emulación; la nota suena al instante

    const std::string wav  = (dir / "ayther_audio_smoke.wav").string();
    const std::string take = (dir / "ayther_audio_smoke.arp").string();
    const Run chip   = measure(core, rom_on,  wav, /*router=*/false, kFrames);
    const Run routed = measure(core, rom_on,  wav, /*router=*/true,  kFrames,
                               take.c_str());
    const Run quiet  = measure(core, rom_off, wav, /*router=*/true,  kFrames);

    std::printf("\n  rms de la salida:  chip=%.1f   router=%.1f   ROM muda=%.1f\n",
                chip.rms, routed.rms, quiet.rms);
    std::printf("  canales que el detector ve vivos:  chip=%u  router=%u (FM=%u)  ROM muda=%u\n",
                chip.active, routed.active, routed.active_fm, quiet.active);

    check(chip.rms > 0.0,
          "NO-VACUIDAD: por el camino del chip, la ROM sintética SUENA");
    check(routed.rms > 0.0,
          "CON EL ROUTER PUESTO —el default— TAMBIÉN SUENA (el Lab estuvo mudo 2 días)");
    // ÉSTE es el check que caza el bug de 2026-08-13, y el 0,25 no es de dedo.
    // El router re-sintetiza el FM con ymfm, así que su nivel no tiene por qué
    // coincidir con el del chip: sano mide 5334 contra 8088 (66%). Con el
    // `addr & 3` viejo puesto de vuelta a mano, el espejo del FM enmudece pero
    // el del PSG sobrevive —ése no pasa por `write_bus`— y quedan 1392 (17%).
    // Por eso «suena» NO alcanza como aserción: el PSG solo tapa la falla del
    // FM entero. Lo que la delata es el NIVEL.
    check(chip.rms <= 0.0 || routed.rms >= chip.rms * 0.25,
          "…y con el NIVEL del chip, no sólo la parte que sobrevive");
    check(quiet.rms >= 0.0 && quiet.rms < 1.0,
          "CONTROL: la misma ROM SIN key-on da silencio (el medidor distingue)");
    // La otra mitad del camino: que el key-on LLEGUE al detector. Es lo que se
    // rompió junto con el sonido, y por el mismo motivo — la convención de
    // `addr`. Sin esto no hay identidad, ni análisis de tomas, ni sustitución.
    check(routed.active > 0 && routed.active_fm > 0,
          "EL DETECTOR VE LA NOTA DE FM (sin esto no hay identidad ni sustitución)");
    check(quiet.active == 0,
          "CONTROL: sin key-on el detector no inventa una voz");

    // ---- El camino de AUTORÍA -------------------------------------------
    // Es otro camino y hay que decirlo aparte: el de arriba mira el detector en
    // vivo, éste el ANÁLISIS de una toma, que es donde el usuario asigna. Esta
    // mañana devolvía 0 eventos sobre una toma perfectamente buena.
    std::printf("\n  análisis de la toma:  %u eventos (FM=%u PSG=%u DAC=%u) · %u con firma\n",
                routed.events, routed.fm_events, routed.psg_events,
                routed.dac_events, routed.with_sig);
    check(routed.events > 0,
          "EL ANÁLISIS DE LA TOMA PRODUCE EVENTOS (es donde se autora)");
    check(routed.with_sig == routed.events && routed.events > 0,
          "…todos con FIRMA, que es lo que se asigna a un asset HD");
    check(routed.fm_events > 0 && routed.psg_events > 0,
          "…y ve los DOS chips del cartucho, no uno solo");
    // El DAC no tiene key-on: se segmenta por silencio, así que un stream
    // continuo es UN bloque. Que sea ≥1 es lo que dice que llegó.
    check(routed.dac_events > 0,
          "EL DAC LLEGA (el registro 0x2A, el que en Golden Axe no aparece)");
    check(quiet.events == 0,
          "CONTROL: la ROM muda no produce un solo evento");

    // ---- Pacing (#146) ---------------------------------------------------
    // «Suena» y «suena BIEN» son cosas distintas: el audio puede llegar entero
    // y aun así entrecortado. Diez causas raíz encadenadas dejaron contadores
    // que hasta hoy no miraba nadie.
    std::printf("\n  pacing:  flush=%llu de %d frames · hambre=%llu · sin salida=%llu\n",
                (unsigned long long)routed.flushed, kFrames,
                (unsigned long long)routed.starved,
                (unsigned long long)routed.inaudible);
    check(routed.flushed >= uint64_t(kFrames) * 9 / 10,
          "EL PCM SALE CASI TODOS LOS FRAMES (no hay gate comiéndose la salida)");
    // Un starve es el router entregando MENOS de lo que el device pidió: se
    // rellena con silencio y eso es un micro-corte. Sano mide 2 en 180 —los del
    // arranque, cuando el resampler todavía no tiene con qué llenar el primer
    // bloque—, así que el umbral es el 5%: lo que se busca no es el corte
    // aislado sino la degradación sistemática, que es la forma que tenía #146.
    check(routed.starved <= uint64_t(kFrames) / 20,
          "…y el router no pasa hambre (cada frame hambriento es un corte)");

    // ---- La SUSTITUCIÓN, que es lo que el producto promete ----------------
    // Tres pasadas sobre la misma toma: sin sustituir, sustituyendo por un WAV
    // MUDO y sustituyendo por uno fuerte. Con las tres se separan las dos
    // mitades de la promesa, que un solo número confunde:
    //
    //   mudo < original      → el original SÍ calla en su ventana
    //   fuerte > mudo        → y el asset SÍ suena en su lugar
    //
    // Sin la pasada muda, un «suena más» podría ser el HD sumándose ENCIMA del
    // original en vez de reemplazarlo — que es un defecto y no una feature.
    if (std::filesystem::exists(take)) {
        const std::string hd_loud  = (dir / "ayther_audio_smoke_hd.wav").string();
        const std::string hd_quiet = (dir / "ayther_audio_smoke_hd_mute.wav").string();
        write_wav(hd_loud,  12000, 44100);
        write_wav(hd_quiet,     0, 44100);
        uint64_t sub_orig = 0, sub_mute = 0;
        const double orig = substitution_rms(core, rom_on, take, nullptr,          wav, true, &sub_orig);
        const double mute = substitution_rms(core, rom_on, take, hd_quiet.c_str(), wav, true, &sub_mute);
        const double loud = substitution_rms(core, rom_on, take, hd_loud.c_str(),  wav);
        // El MISMO par por el camino viejo. No es redundancia: es lo que
        // convierte «no calla» en «no calla CON EL ROUTER», que es la mitad del
        // diagnóstico — y lo que dice que el escenario del test es correcto.
        const double orig_old = substitution_rms(core, rom_on, take, nullptr,          wav, false);
        const double mute_old = substitution_rms(core, rom_on, take, hd_quiet.c_str(), wav, false);
        std::printf("\n  sustitución (router puesto): original=%.1f  HD mudo=%.1f  HD fuerte=%.1f\n"
                    "  sustitución (router sacado ): original=%.1f  HD mudo=%.1f\n"
                    "  voces que el router NO copió:  sin sustituir=%llu  sustituyendo=%llu\n",
                    orig, mute, loud, orig_old, mute_old,
                    (unsigned long long)sub_orig, (unsigned long long)sub_mute);
        check(orig > 0.0, "NO-VACUIDAD: la toma reproducida sin sustituir suena");
        check(loud > mute, "EL ASSET HD SUENA en lugar del evento asignado");
        check(mute_old >= 0.0 && mute_old < orig_old,
              "EL ORIGINAL CALLA cuando lo sustituyen (camino sin router)");
        // El mismo par POR EL CAMINO DEL ROUTER, que es el default. Este check
        // encontró #414 el 2026-08-14 —`mute` daba EXACTAMENTE `orig`— y quedó
        // escrito y desactivado hasta que se arreglara. Ya está: la causa era
        // que el cebado tras un salto se llevaba puestas las escrituras del
        // frame al que se saltaba, o sea su key-on, y sin key-on la política
        // del router nunca se consulta (ver voice_tick).
        check(mute >= 0.0 && mute < orig,
              "EL ORIGINAL CALLA cuando lo sustituyen (camino del router, #414)");
        // Y que calle porque la DECISIÓN llegó al router, no porque el canal se
        // haya quedado mudo por el camino — que es exactamente la forma que
        // tenía #414: el nivel bajaba con el asset puesto y aun así ninguna voz
        // había sido silenciada. Un RMS más chico no distingue las dos cosas.
        check(sub_orig == 0 && sub_mute > 0,
              "…y calla por DECISIÓN: sin asignar no se silencia ninguna voz, "
              "asignando sí (#414)");
    } else {
        check(false, "no se pudo guardar la toma para probar la sustitución");
    }

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
