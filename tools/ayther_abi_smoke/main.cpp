// ---------------------------------------------------------------------------
// ayther_abi_smoke (E-1, #397) — la negociación de la ABI AYTHER v1.
//
// POR QUÉ EXISTE. Hasta acá todo el diálogo con el core iba por
// `retro_get_memory_data(0x100-0x10E)`: punteros mutables crudos, sin versión y
// sin forma de preguntar qué hay del otro lado. La ABI v1 agrega esa pregunta, y
// lo que este oráculo fija es que la respuesta se interprete bien EN LOS DOS
// CASOS — porque el que importa no es el feliz:
//
//   · con el fork  → has_ayther_v1() == true, y las capabilities declaradas
//     incluyen SUBSCRIPTIONS_V1 (lo que E-2 va a necesitar);
//   · con un core STOCK → has_ayther_v1() == false y la carga IGUAL tiene
//     éxito. Ése es el criterio de fondo de E-1: la negociación es aditiva, así
//     que fallar en resolver el símbolo no puede romper a nadie. Si esto se
//     rompiera, el Lab dejaría de abrir con cualquier core que no sea el fork.
//
// El caso positivo necesita una DLL con la ABI, que no vive en el repo (la
// compila el fork). Se pasa por AYTHER_ABI_CORE; sin esa variable el oráculo
// verifica el caso stock —que es el que protege contra la regresión— y avisa
// que el otro quedó sin ejercitar, en vez de pasar en silencio.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target ayther_abi_smoke
//   Env:   AYTHER_ABI_CORE   = DLL del fork con ABI v1 (opcional)
//          AYTHER_STOCK_CORE = DLL sin ABI (opcional; default: la del repo)
//          AYTHER_PROBE_ROM  = ROM para init() (opcional: sin ella corre con la
//                              ROM sintética del repo)
// ---------------------------------------------------------------------------
#include "../common/synth_rom.h"

#include "libretro_host/retro_runner.h"
#include "ayther_session.h"
#include "ayther_env.h"

#include <cstring>

// El control usa a propósito los accessors legacy: son la VERDAD contra la que
// se compara el camino nuevo, así que su deprecación no aplica acá.
#define AYTHER_LEGACY_SPRITES_BEGIN     _Pragma("clang diagnostic push")     _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define AYTHER_LEGACY_SPRITES_END _Pragma("clang diagnostic pop")

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

std::string env_or(const char* key, const std::string& def) {
    if (const char* v = ayther::env_get(key)) if (*v) return v;
    return def;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string root = AYTHER_SOURCE_DIR;

    std::printf("=== ayther_abi_smoke (E-1 #397) — negociacion de la ABI v1 ===\n");

    // #415: la ROM del entorno si la hay, y si no la SINTÉTICA del repo. `roms/`
    // está en .gitignore, así que pedir una comercial dejaba este test sin
    // registrar en toda máquina que no fuera la del autor — CI incluida.
    const std::string rom = ayther::synth::probe_rom_path();
    if (rom.empty() || !fs::exists(rom)) {
        std::printf("[skip] no se pudo preparar una ROM para init()\n");
        return 77;
    }

    // ---- Caso STOCK: sin ABI, pero la carga funciona igual -----------------
    const std::string stock = env_or(
        "AYTHER_STOCK_CORE", root + "/third_party/cores/genesis_plus_gx_libretro.dll");
    if (fs::exists(stock)) {
        std::printf("\ncore stock: %s\n", stock.c_str());
        RetroRunner r;
        const bool loaded = r.init(stock, rom);
        check(loaded, "un core SIN la ABI carga igual (la negociacion es aditiva)");
        check(!r.has_ayther_v1(), "…y has_ayther_v1() es false");
        check(r.ayther_api() == nullptr, "…y el descriptor es nullptr");
        if (loaded) {
            r.shutdown();
            check(!r.has_ayther_v1(), "tras shutdown sigue en false (punteros limpios)");
        }
    } else {
        std::printf("\n[aviso] no hay core stock en %s — caso negativo SIN ejercitar\n",
                    stock.c_str());
    }

    // ---- Caso FORK: la ABI negocia y declara sus capabilities --------------
    const std::string fork = env_or("AYTHER_ABI_CORE", "");
    if (fork.empty() || !fs::exists(fork)) {
        std::printf("\n[aviso] sin AYTHER_ABI_CORE — el caso POSITIVO quedo sin\n"
                    "        ejercitar. Apuntarlo a la DLL del fork (>= f3fe1e1).\n");
    } else {
        std::printf("\ncore con ABI: %s\n", fork.c_str());
        RetroRunner r;
        const bool loaded = r.init(fork, rom);
        check(loaded, "el core del fork carga");
        check(r.has_ayther_v1(), "has_ayther_v1() es true");
        const ayther_interface_v1* api = r.ayther_api();
        check(api != nullptr, "…y el descriptor llega");
        if (api) {
            check((api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1) != 0,
                  "declara SUBSCRIPTIONS_V1 (lo que E-2 necesita)");
            check(api->build_id != nullptr && api->build_id_size > 0,
                  "declara un build_id no vacio (trazabilidad del core)");
            std::printf("       build_id=%.*s  caps=0x%016llX\n",
                        static_cast<int>(api->build_id_size), api->build_id,
                        static_cast<unsigned long long>(api->capabilities));
        }
        if (loaded) {
            r.shutdown();
            check(!r.has_ayther_v1() && r.ayther_api() == nullptr,
                  "shutdown suelta el descriptor (muere con el modulo)");
        }
    }

    // -----------------------------------------------------------------------
    // ABI 1.10: SYSTEM dice de qué frame habla. `h40` describe el frame EMITIDO
    // (en 1.9 salía de reg 12 y en el frame de transición contradecía al
    // viewport: Sonic 2 escribe H40 en el primer frame, que sale 256x192), y
    // GEOMETRY_PENDING marca exactamente los frames en que registros y frame
    // emitido difieren. El oráculo: en NINGÚN frame h40 contradice al viewport,
    // y el flag se apaga (la geometría se estabiliza) con vdp_mode ya elegido.
    // -----------------------------------------------------------------------
    if (!fork.empty() && fs::exists(fork)) {
        std::printf("\n--- ABI 1.10: SYSTEM vs frame emitido ---\n");
        RetroRunner r;
        if (r.init(fork, rom) && r.has_ayther_v1()) {
            const uint32_t v = r.ayther_api()->abi_version;
            if (v < AYTHER_ABI_VERSION_1_10) {
                std::printf("[aviso] core ABI %u.%u < 1.10 — sin ejercitar\n",
                            AYTHER_ABI_VERSION_MAJOR(v), AYTHER_ABI_VERSION_MINOR(v));
            } else {
        int contradictions = 0, pending_count = 0, first_pending = -1;
                int settled_at = -1;
                ayther_system_v1 sys{};
                for (int i = 0; i < 120; ++i) {
                    r.run_frame();
                    if (!r.read_system_v1(sys).ok()) { contradictions = 1 << 30; break; }
                    const bool pending = (sys.flags & AYTHER_SYSTEM_GEOMETRY_PENDING) != 0;
                    if ((sys.h40 != 0) != (sys.viewport_w == 320)) ++contradictions;
            if (pending) { ++pending_count; if (first_pending < 0) first_pending = i; }
                    else if (settled_at < 0 && sys.vdp_mode != 0) settled_at = i;
                    if (i < 3)
                        std::printf("       f%d: vdp_mode=%u h40=%u viewport=%ux%u pending=%d\n",
                                    i, sys.vdp_mode, sys.h40, sys.viewport_w, sys.viewport_h,
                                    pending ? 1 : 0);
                }
                check(contradictions == 0,
                      "h40 nunca contradice al viewport (describe el frame emitido)");
                check(settled_at >= 0,
                      "GEOMETRY_PENDING se apaga con vdp_mode elegido (geometria definitiva)");
                std::printf("       frames pendientes=%d (primero=%d)  estable desde f%d\n",
                    pending_count, first_pending, settled_at);
            }
            r.shutdown();
        } else {
            check(false, "el core del fork carga para el oraculo de SYSTEM 1.10");
        }
    }

    // -----------------------------------------------------------------------
    // E-2 (#398): las SUSCRIPCIONES, ya a nivel AytherSession.
    //
    // El fork compila con el perfil estándar: ningún subsistema de observación
    // trabaja hasta que el frontend declare qué necesita. Lo que se verifica es
    // que la sesión las pida, que el core las active en el frame boundary, y
    // —lo que de verdad protege— que un reset no las deje mudas en silencio.
    // -----------------------------------------------------------------------
    std::printf("\n--- E-2: suscripciones ---\n");
    {
        ayther::AytherSession::Config c;
        c.rom_path = rom;
        c.enable_audio = false;

        // Caso STOCK: no-op limpio.
        if (fs::exists(stock)) {
            c.core_path = stock;
            auto r = ayther::AytherSession::create(c);
            check(!!r, "la sesion se crea con un core SIN ABI");
            if (r) {
                auto& s = *r;
                uint32_t req = 1, act = 1, sup = 1;
                s->ayther_subscriptions(&req, &act, &sup);
                check(!s->has_ayther_abi(), "has_ayther_abi() false con core stock");
                check(req == 0 && act == 0 && sup == 0,
                      "…y las tres mascaras en 0 (no-op, sin error)");
                s->step();
                check(true, "un frame corre normal (sin crash ni ruido)");
            }
        }

        // Caso FORK: pide, el core activa, y el reset las repone.
        if (!fork.empty() && fs::exists(fork)) {
            c.core_path = fork;
            auto r = ayther::AytherSession::create(c);
            check(!!r, "la sesion se crea con el core del fork");
            if (r) {
                auto& s = *r;
                uint32_t req = 0, act = 0, sup = 0;
                s->ayther_subscriptions(&req, &act, &sup);
                check(s->has_ayther_abi(), "has_ayther_abi() true con el fork");
                check(req != 0, "la sesion PIDIO suscripciones al iniciar");
                // ABI 1.9: la sesión pide lo que CONSUME, no AYTHER_SUB_ALL —
                // que ahora incluye ATTRIBUTION, LINE_* y FRAME_HASH, con
                // costo por frame y sin lector en el Engine (guía §4).
                check(req == (RetroRunner::kEngineSubscriptions & sup),
                      "pidio lo que el Engine consume, acotado a lo soportado");
                check((req & ~RetroRunner::kEngineSubscriptions) == 0,
                      "no pidio ninguna suscripcion que no lee (1.9: ATTRIBUTION, "
                      "LINE_STATE/CRAM/CELLS, FRAME_HASH)");
                if (sup & ~UINT32_C(0x7F))
                    check((req & AYTHER_SUB_FRAME_HASH) == 0,
                          "el core ofrece FRAME_HASH y la sesion NO lo pide "
                          "(~100 KB/frame sin consumidor)");
                std::printf("       pedidas=0x%08X  soportadas=0x%08X  activas(pre)=0x%08X\n",
                            req, sup, act);

                s->step();
                s->ayther_subscriptions(&req, &act, &sup);
                check(act == req, "tras el PRIMER frame, activas == pedidas");
                std::printf("       activas=0x%08X\n", act);

                // El reset del core limpia las suscripciones; la sesion tiene
                // que reponerlas o todo lo observado por la ABI queda mudo.
                s->reset();
                s->step();
                uint32_t act2 = 0;
                s->ayther_subscriptions(nullptr, &act2, nullptr);
                check(act2 == req, "tras un RESET vuelven a quedar activas");
                std::printf("       activas tras reset=0x%08X\n", act2);
            }
        }
    }

    // -----------------------------------------------------------------------
    // E-2: activar suscripciones NO puede alterar la emulación.
    //
    // El criterio del issue lo delega a `full_core_replay`, que vive en el repo
    // del fork. Lo que sí se puede medir desde acá —y es el riesgo concreto—
    // es que con los subsistemas de observación PRENDIDOS el core siga
    // corriendo igual: dos sesiones idénticas, N frames, misma RAM. Si activar
    // suscripciones metiera un efecto en la emulación, esto lo caza sin salir
    // de este repo.
    // -----------------------------------------------------------------------
    if (!fork.empty() && fs::exists(fork)) {
        std::printf("\n--- E-2: la emulacion no cambia con las suscripciones ---\n");
        constexpr int kFrames = 120;
        auto run_hash = [&](uint64_t* out) -> bool {
            ayther::AytherSession::Config c;
            c.core_path = fork; c.rom_path = rom; c.enable_audio = false;
            auto r = ayther::AytherSession::create(c);
            if (!r) return false;
            auto& s = *r;
            for (int i = 0; i < kFrames; ++i) s->step();
            const uint8_t* ram = s->work_ram();
            const size_t   n   = s->work_ram_size();
            if (!ram || !n) return false;
            uint64_t h = 1469598103934665603ULL;          // FNV-1a
            for (size_t i = 0; i < n; ++i) { h ^= ram[i]; h *= 1099511628211ULL; }
            *out = h;
            return true;
        };
        uint64_t h1 = 0, h2 = 0;
        const bool ok1 = run_hash(&h1), ok2 = run_hash(&h2);
        check(ok1 && ok2, "dos sesiones de 120 frames con el fork corren");
        check(ok1 && ok2 && h1 == h2,
              "…y dejan la MISMA RAM (suscripciones activas = emulacion intacta)");
        std::printf("       ram_hash=%016llX\n", (unsigned long long)h1);
    }

    // ---- Que MAS limpia las suscripciones, ademas de reset? ----------------
    // E-2 descubrio que `retro_reset` las borra y por eso la sesion las repone.
    // Pero el compose hace `unserialize` en CADA frame —el shadow, o el propio
    // runner en el camino in-place—, y nadie habia preguntado si eso tambien
    // las limpia. Si lo hiciera, la observacion entera se apagaria despues del
    // primer compose y en SILENCIO: el fork sin suscripcion devuelve cero, no
    // error. Se verifica en vez de suponerlo.
    if (!fork.empty() && fs::exists(fork)) {
        std::printf("\n--- unserialize vs suscripciones ---\n");
        RetroRunner r;
        if (r.init(fork, rom) && r.has_ayther_v1()) {
            const ayther_interface_v1* a = r.ayther_api();
            ayther_subscription_state_v1 st{};
            st.struct_size = sizeof(st);
            a->get_subscriptions(&st, sizeof(st));
            const uint32_t want = AYTHER_SUB_ALL & st.supported_mask;
            a->set_subscriptions(want);
            for (int i = 0; i < 120; ++i) r.run_frame();

            std::vector<uint8_t> save;
            const bool ser = r.serialize(save);
            check(ser, "el core serializa (precondicion del compose)");
            st = {}; st.struct_size = sizeof(st);
            a->get_subscriptions(&st, sizeof(st));
        const uint32_t previous_mask = st.active_mask;

            const bool unser = ser && r.unserialize(save);
            check(unser, "…y unserializa");
            r.run_frame();                       // las mascaras entran en el boundary
            st = {}; st.struct_size = sizeof(st);
            a->get_subscriptions(&st, sizeof(st));
        check(st.active_mask == previous_mask,
                  "unserialize NO apaga las suscripciones (el compose las usa)");
            std::printf("       activas antes=0x%08X  despues=0x%08X\n",
                    previous_mask, st.active_mask);
            r.shutdown();
        }
    }

    // ---- E-5 (#401): lo que la SESION PUBLICA, no lo que el runner lee -----
    // El hueco que dejó pasar el glitch de sprites del viewport: `abi_read_parity`
    // compara `read_parsed_sprites_v1` contra el puntero legacy y da 17/17,
    // pero nadie miraba `AytherSession::parsed_sprites_raw()` — el que consume
    // el renderer. Ahí el count publicado era `abi_sprites.size()` (el BUFFER,
    // dimensionado con el count del snapshot) en vez de las entradas realmente
    // leídas, así que la cola sin llenar viajaba al viewport como sprites y se
    // dibujaba con patrones basura. Verde en todos lados, roto en pantalla.
    //
    // Control: un RetroRunner CRUDO con la misma ROM y los mismos frames sin
    // input — determinista, así que su lista legacy es la verdad contra la que
    // se compara lo que la sesión publica.
    if (!fork.empty() && fs::exists(fork)) {
        std::printf("\n--- E-5: lo que publica la sesion vs el legacy ---\n");
        ayther::AytherSession::Config cfg;
        cfg.core_path = fork; cfg.rom_path = rom; cfg.enable_audio = false;
        auto rs = ayther::AytherSession::create(cfg);
        RetroRunner ctrl;
        const bool ctrl_ok = ctrl.init(fork, rom);
        check(rs && ctrl_ok, "sesion + runner de control arrancan con el fork");
        // El control TAMBIEN tiene que suscribirse: con el perfil estandar el
        // fork no parsea sprites hasta que alguien los pida, y sin esto la
        // lista legacy queda en cero — un control mudo que haria pasar
        // cualquier cosa (el chequeo de no-vacuidad de abajo lo destapo).
        if (ctrl_ok && ctrl.has_ayther_v1()) {
            const ayther_interface_v1* capi = ctrl.ayther_api();
            ayther_subscription_state_v1 st{};
            st.struct_size = sizeof(st);
            capi->get_subscriptions(&st, sizeof(st));
            capi->set_subscriptions(AYTHER_SUB_ALL & st.supported_mask);
        }
        if (rs && ctrl_ok) {
            auto& s = *rs;
            int frames_with_sprites = 0, discrepancies = 0, max_published = 0;
            for (int i = 0; i < 900; ++i) {
                s->step();
                ctrl.run_frame();
                uint8_t pub_n = 0;
                const uint8_t* pub = s->parsed_sprites_raw(&pub_n);
                AYTHER_LEGACY_SPRITES_BEGIN
                const uint8_t  leg_n = ctrl.parsed_sprite_count();
                const uint8_t* leg   = ctrl.parsed_sprites();
                AYTHER_LEGACY_SPRITES_END
                if (pub_n > max_published) max_published = pub_n;
                if (leg_n) ++frames_with_sprites;
                if (pub_n != leg_n) { ++discrepancies; continue; }
                if (!pub || !leg) continue;
                // 10 bytes por entrada; comparar el CONTENIDO, no sólo el count:
                // un count correcto con entradas sin llenar es el mismo defecto.
                if (std::memcmp(pub, leg, (size_t)pub_n * 10) != 0) ++discrepancies;
            }
            check(frames_with_sprites > 0,
                  "el tramo tiene sprites (si no, el check seria VACUO)");
            check(max_published > 0, "la sesion publica sprites en algun frame");
            check(discrepancies == 0,
                  "lo que publica la sesion == el legacy, count Y contenido");
            std::printf("       frames con sprites=%d  max publicados=%d  "
                        "discrepancias=%d\n",
                        frames_with_sprites, max_published, discrepancies);
            ctrl.shutdown();
        }
    }

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    if (g_fails) return 1;
    // Sin el core del fork, lo que corrio arriba es SOLO el caso negativo (que un
    // core sin ABI carga igual). Devolver 0 lo contaria como un test que paso, y
    // este oraculo existe para medir la NEGOCIACION — o sea justo lo que no se
    // pudo ejercitar. 77 = ctest lo reporta como SALTEADO (#415): el verde no
    // tiene que poder mentir por omision.
    if (fork.empty() || !fs::exists(fork)) {
        std::printf("[skip] sin el core del fork, el caso POSITIVO no se midio\n");
        return 77;
    }
    return 0;
}
