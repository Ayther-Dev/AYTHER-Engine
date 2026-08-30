// ---------------------------------------------------------------------------
// video_stream_smoke — oráculo del video POR STREAMING desde el pack (#375).
//
// QUÉ AFIRMA #375, y por qué cada afirmación necesita medida:
//
//  1. LA RESIDENCIA NO DEPENDE DEL TAMAÑO DEL VIDEO. Es la tesis entera: el
//     tope de 32 MB por video del bake existía porque `ayther_pack_read` es
//     todo-o-nada y el clip se materializaba entero. Se retiró el tope, así
//     que hay que poder mostrar que un clip abierto ocupa un paquete y un
//     frame. Sin número medido, «streaming» es una palabra — y el tope se
//     habría sacado a fe. Se compara contra la lectura entera del MISMO clip.
//
//  2. LOS BYTES SON LOS MISMOS. Leer por rango pasa por el FFI, por la
//     verificación por trozo y por el offset del ZIP; cualquiera de los tres
//     puede estar corrido un byte y el síntoma sería un video que decodifica
//     casi bien. Se compara rango por rango contra el contenido conocido,
//     cruzando bordes de trozo (65536) a propósito.
//
//  3. LAS TRES FUENTES COINCIDEN. Pack por rango, archivo del disco y buffer
//     en RAM tienen que entregar lo mismo: son los tres caminos que toma la
//     sesión (pack lazy, autoría contra el proyecto, pack legacy).
//
//  4. EL VALIDADOR LEE A TRAVÉS DE LA FUENTE. Rechaza un IVF cuyos paquetes no
//     son VP9 sin leerse el archivo entero — es lo que permite que el bake
//     valide un master de 8K sin reservar un giga.
//
// MATERIAL: se sintetiza acá. El IVF que se genera es estructuralmente válido
// (header DKIF + N paquetes con su tamaño) pero sus payloads son relleno, no
// VP9. Alcanza y sobra para 1-4, que son afirmaciones de TRANSPORTE, y hace que
// el oráculo corra en cualquier máquina sin ffmpeg ni material del proyecto.
//
// Con un `.ivf` all-intra REAL como argumento agrega la prueba que el relleno
// no puede dar: que los PÍXELES decodificados por streaming son byte por byte
// los mismos que por lectura entera, frame por frame.
//
// Uso:  video_stream_smoke [all_intra.ivf]
// ---------------------------------------------------------------------------
#include "ayther_video.h"
#include "ayther_core_ffi.h"
#include "trusted_pack_fixture.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

void rd_le32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
}
void rd_le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}

/// IVF estructuralmente válido con `frames` paquetes de `payload` bytes de
/// relleno determinista. El demuxer sólo lee los TAMAÑOS, así que esto indexa
/// igual que un video real; lo que no se puede es decodificarlo.
std::vector<uint8_t> synth_ivf(uint32_t frames, uint32_t payload,
                               uint16_t w, uint16_t h, uint32_t rate, uint32_t scale) {
    std::vector<uint8_t> v;
    v.insert(v.end(), { 'D', 'K', 'I', 'F' });
    rd_le16(v, 0);            // version
    rd_le16(v, 32);           // header length
    v.insert(v.end(), { 'V', 'P', '9', '0' });
    rd_le16(v, w);
    rd_le16(v, h);
    rd_le32(v, rate);
    rd_le32(v, scale);
    rd_le32(v, frames);
    rd_le32(v, 0);            // unused
    for (uint32_t i = 0; i < frames; ++i) {
        rd_le32(v, payload);
        rd_le32(v, i);        // pts lo
        rd_le32(v, 0);        // pts hi
        for (uint32_t k = 0; k < payload; ++k)
            v.push_back(uint8_t((i * 31u + k * 7u) % 251u));
    }
    return v;
}

bool write_file(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    return ok;
}

std::vector<uint8_t> read_file(const std::string& p) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(size_t(n));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

/// Hornea un `.ay` firmado con un solo asset de video.
bool bake_pack(ayther::test::TrustedPackFixture& fixture,
               const std::string& entry, const std::vector<uint8_t>& ivf) {
    static const char* kManifest =
        "[pack]\nname = \"stream smoke\"\nversion = \"1.0.0\"\n"
        "game_id = \"golden_axe\"\nayther_min = \"0.8.0\"\n"
        "[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
    bool ok = fixture.add_bytes(
                  "manifest.toml",
                  reinterpret_cast<const uint8_t*>(kManifest), std::strlen(kManifest))
           && fixture.add_bytes(entry.c_str(), ivf.data(), ivf.size());
    char err[256] = {};
    if (ok) ok = fixture.finish(err, sizeof(err));
    if (!ok && err[0]) std::printf("       bake: %s\n", err);
    return ok;
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t).count();
}

const char* human(uint64_t n, char* buf, size_t cap) {
    if (n >= 1024 * 1024) std::snprintf(buf, cap, "%.1f MB", double(n) / (1024 * 1024));
    else                  std::snprintf(buf, cap, "%.1f KB", double(n) / 1024);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::printf("=== video_stream_smoke — video por rango desde el pack (#375) ===\n");

    if (!ayther::VideoClip::available()) {
        std::printf("[skip] engine sin libvpx — nada que medir\n=== ok ===\n");
        return 0;
    }

    const fs::path dir = fs::temp_directory_path() / "ayther_stream_smoke";
    std::error_code ec;
    fs::create_directories(dir, ec);

    // ---- Material sintético: 40 frames × 200 KB ≈ 7,6 MB --------------------
    // Grande a propósito: con un clip de pocos KB la diferencia de residencia
    // entre streaming y lectura entera se perdería en los buffers fijos.
    const uint32_t kFrames = 40, kPayload = 200 * 1024;
    const std::vector<uint8_t> ivf = synth_ivf(kFrames, kPayload, 320, 224, 30, 1);
    const fs::path ivf_path = dir / "clip.ivf";
    ayther::test::TrustedPackFixture pack_fixture{"video_stream"};
    check(write_file(ivf_path, ivf), "se escribio el IVF sintetico");
    check(bake_pack(pack_fixture, "video/clip.ivf", ivf), "se horneo el pack firmado");

    AyArchive* pack = pack_fixture.open();
    check(pack != nullptr, "el pack abre y verifica su firma");
    if (!pack) return 1;

    char hb[64], hb2[64];
    std::printf("       clip sintetico: %s en %u frames\n",
                human(ivf.size(), hb, sizeof(hb)), kFrames);

    // ---- 3.a La entrada es direccionable por rango -------------------------
    check(ayther_pack_entry_streamable(pack, "video/clip.ivf"),
          "el .ivf del pack se declara direccionable por rango");
    // Y la capa de video CAE al camino viejo cuando no lo es, en vez de dar una
    // fuente a medias: es lo que mantiene reproducibles los packs anteriores
    // a #375 y las entradas deflateadas.
    check(ayther::video_source_from_pack(pack, "manifest.toml") == nullptr,
          "una entrada NO direccionable no devuelve fuente (el llamador cae a read)");
    check(ayther::video_source_from_pack(pack, "video/no_existe.ivf") == nullptr,
          "una entrada ausente tampoco");

    // ---- 2. Los bytes por rango son los del contenido ---------------------
    {
        auto src = ayther::video_source_from_pack(pack, "video/clip.ivf");
        check(src != nullptr, "hay fuente por rango sobre el pack");
        if (src) {
            check(src->size() == ivf.size(), "la fuente declara el tamano real");
            check(src->resident_bytes() == 0,
                  "la fuente del pack no retiene bytes");

            // Rangos elegidos para cruzar el borde de trozo (65536) y para caer
            // justo en el ultimo byte: ahi se veria un off-by-one.
            struct R { uint64_t off; size_t len; } rs[] = {
                { 0, 32 }, { 12, 187 }, { 65530, 12 }, { 65536, 64 },
                { 65535, 2 }, { ivf.size() - 1, 1 },
                { ivf.size() / 2, 4096 },
            };
            bool all = true;
            for (const R& r : rs) {
                std::vector<uint8_t> got(r.len, 0);
                if (!src->read(r.off, r.len, got.data()) ||
                    std::memcmp(got.data(), ivf.data() + r.off, r.len) != 0) {
                    std::printf("       rango %llu+%zu difiere\n",
                                (unsigned long long)r.off, r.len);
                    all = false;
                }
            }
            check(all, "cada rango leido coincide con el contenido (bordes de trozo incluidos)");

            // Un rango que se pasa del final: la fuente lo rechaza en vez de
            // entregar basura (el demuxer distingue por el tamano total).
            uint8_t one = 0;
            check(!src->read(ivf.size(), 1, &one),
                  "un rango fuera del final se rechaza");
        }
    }

    // ---- 1. Residencia: streaming vs lectura entera -----------------------
    uint64_t res_stream = 0, res_whole = 0;
    {
        ayther::VideoClip clip;
        std::string err;
        const bool ok = clip.open(
            ayther::video_source_from_pack(pack, "video/clip.ivf"), &err);
        if (!ok) std::printf("       motivo: %s\n", err.c_str());
        check(ok, "el clip abre POR STREAMING desde el pack");
        check(clip.frame_count() == kFrames, "indexo los 40 frames leyendo por bloques");
        check(clip.width() == 320 && clip.height() == 224, "dimensiones del header IVF");
        check(clip.fps() > 29.9 && clip.fps() < 30.1, "time base del header IVF");
        res_stream = clip.resident_bytes();
    }
    {
        ayther::VideoClip clip;
        std::string err;
        check(clip.open(ivf.data(), ivf.size(), &err),
              "el clip abre por lectura entera (camino de los packs legacy)");
        check(clip.frame_count() == kFrames, "mismo indice por las dos vias");
        res_whole = clip.resident_bytes();
    }
    std::printf("       residencia: streaming %s  ·  lectura entera %s\n",
                human(res_stream, hb, sizeof(hb)),
                human(res_whole, hb2, sizeof(hb2)));

    // El clip por streaming se queda con UN paquete (200 KB) mas el frame BGRA
    // (320×224×4 = 280 KB) mas el indice. La lectura entera se queda, ademas,
    // con los 7,6 MB del archivo. El margen del cuarto es holgado a proposito:
    // lo que se afirma es que no ESCALA con el video, no un numero exacto.
    check(res_stream < ivf.size() / 4,
          "la residencia por streaming no escala con el tamano del video");
    check(res_whole >= ivf.size(),
          "la lectura entera si retiene el archivo (control del test)");

    // ---- 3.b Las tres fuentes coinciden -----------------------------------
    {
        ayther::VideoClip a, b;
        std::string e1, e2;
        const bool oa = a.open(ayther::video_source_from_file(ivf_path.string()), &e1);
        const bool ob = b.open(ayther::video_source_from_pack(pack, "video/clip.ivf"), &e2);
        check(oa && ob, "abre igual desde el disco y desde el pack");
        check(a.frame_count() == b.frame_count() &&
              a.width() == b.width() && a.height() == b.height(),
              "disco y pack dan el mismo indice y las mismas dimensiones");
        // La fuente de archivo tampoco retiene: es la que usa la AUTORIA, donde
        // no hay pack y el asset del paso es una ruta absoluta al .ivf.
        check(a.resident_bytes() < ivf.size() / 4,
              "la fuente de archivo tampoco retiene el clip");
    }

    // ---- 5. La medicion de costo NO inventa (#377) -------------------------
    //
    // El clip sintetico abre (el contenedor esta bien) pero sus paquetes no son
    // VP9, asi que ningun frame decodifica. `video_probe` tiene que devolver
    // false: un aviso de rendimiento sobre algo que no se pudo medir mandaria al
    // artista a degradar su master por nada.
    {
        ayther::VideoClip clip;
        std::string err;
        check(clip.open(ayther::video_source_from_pack(pack, "video/clip.ivf"), &err),
              "el clip sintetico abre (el contenedor es valido)");
        ayther::VideoProbe p{};
        check(!ayther::video_probe(clip, 8, &p),
              "medir un clip que no decodifica devuelve false, no un numero");
        check(p.sampled == 0, "y no deja muestras a medias");
    }

    // ---- 4. El validador lee a traves de la fuente ------------------------
    {
        auto src = ayther::video_source_from_pack(pack, "video/clip.ivf");
        ayther::VideoInfo info{};
        std::string why;
        const bool valid = ayther::VideoClip::validate(*src, &info, &why);
        check(!valid, "el validador RECHAZA paquetes que no son VP9");
        check(info.frames == kFrames && !info.all_keyframes,
              "pero reporta lo que si pudo leer del contenedor");
        if (!why.empty()) std::printf("       motivo: %s\n", why.c_str());
    }

    // ---- Con material real: los PIXELES coinciden -------------------------
    if (argc >= 2) {
        const std::vector<uint8_t> real = read_file(argv[1]);
        check(!real.empty(), std::string("se pudo leer ") + argv[1]);
        if (!real.empty()) {
            ayther::test::TrustedPackFixture real_fixture{"video_stream_real"};
            check(bake_pack(real_fixture, "video/real.ivf", real),
                  "se horneo un pack con el clip real");
            AyArchive* rpack = real_fixture.open();
            check(rpack != nullptr, "el pack del clip real abre");
            if (rpack) {
                ayther::VideoClip s, w;
                std::string e1, e2;
                // COSTO DEL BARRIDO. Es el número que decide si hace falta
                // hornear el índice de paquetes como sidecar: el barrido lee y
                // verifica el archivo una vez al abrir, así que crece con el
                // tamaño del clip aunque la residencia no lo haga. Si un tier
                // 8K llevara esto a segundos, el sidecar deja de ser opcional.
                const auto t_open = std::chrono::steady_clock::now();
                const bool os_ = s.open(
                    ayther::video_source_from_pack(rpack, "video/real.ivf"), &e1);
                const double open_ms = ms_since(t_open);
                // #379: LO MISMO CON EL ÍNDICE HORNEADO. Es la comparación que
                // justifica (o no) el sidecar: mismo clip, misma fuente, la
                // única diferencia es si hay que barrer para saber dónde está
                // cada frame. El índice se construye acá con la misma función
                // que usa el bake.
                double idx_ms = -1.0;
                {
                    std::vector<uint8_t> idx;
                    std::string iwhy;
                    auto bsrc = ayther::video_source_from_pack(rpack, "video/real.ivf");
                    if (bsrc && ayther::video_index_build(*bsrc, &idx, &iwhy)) {
                        ayther::VideoClip q;
                        std::string qe;
                        const auto t_idx = std::chrono::steady_clock::now();
                        const bool oq = q.open(
                            ayther::video_source_from_pack(rpack, "video/real.ivf"),
                            idx.data(), idx.size(), &qe);
                        idx_ms = ms_since(t_idx);
                        check(oq && q.frame_count() == s.frame_count(),
                              "con índice horneado abre y da los MISMOS frames");
                    }
                }
                const bool ow = w.open(real.data(), real.size(), &e2);
                check(os_ && ow, "el clip real abre por las dos vias");
                check(s.frame_count() == w.frame_count(),
                      "misma cantidad de frames por las dos vias");

                // RESIDENCIA, medida ANTES de decodificar nada. Recien abiertos,
                // los dos clips tienen el buffer de paquete vacio, asi que la
                // unica diferencia entre ellos es el archivo — y eso se puede
                // afirmar como igualdad exacta. Medirla despues de decodificar no
                // serviria: el buffer de paquete crece segun el ORDEN de tamanos
                // que le toco ver, asi que dos clips con distinta historia de
                // decode tienen capacidades distintas por razones que no son el
                // streaming.
                char hb3[64];
                // #378: el frame ya no reside; lo que se informa es lo que se
                // SUBE por frame, 1,5 bytes por pixel en vez de 4.
                const uint64_t frame_bytes = uint64_t(s.width()) * s.height() * 3 / 2;
                std::printf("       clip real: %ux%u, %u frames, archivo %s\n",
                            s.width(), s.height(), s.frame_count(),
                            human(real.size(), hb3, sizeof(hb3)));
                std::printf("       barrido del indice: %.1f ms (%.0f MB/s) — "
                            "una pasada leyendo y verificando, al abrir\n",
                            open_ms,
                            double(real.size()) / (1024.0 * 1024.0) / (open_ms / 1000.0));
                if (idx_ms >= 0.0)
                    std::printf("       con .ivf.idx horneado: %.1f ms (x%.0f) — "
                                "12 bytes por frame en vez de la pasada (#379)\n",
                                idx_ms, idx_ms > 0.0 ? open_ms / idx_ms : 0.0);
                std::printf("       residencia recien abierto: streaming %s (de "
                            "el frame I420 que se SUBE son %s, y ya no reside)  ·  entero %s\n",
                            human(s.resident_bytes(), hb, sizeof(hb)),
                            human(frame_bytes, hb3, sizeof(hb3)),
                            human(w.resident_bytes(), hb2, sizeof(hb2)));
                check(w.resident_bytes() - s.resident_bytes() == real.size(),
                      "la diferencia entre las dos vias es EXACTAMENTE el archivo");
                check(s.resident_bytes() < w.resident_bytes(),
                      "y el streaming pesa menos que la lectura entera");

                // #377: el COSTO, medido antes de la comparacion frame por frame.
                // Es el numero con el que el Lab avisa al artista, y el Lab lo
                // mide sobre un clip recien abierto: medirlo despues de 658
                // decodes daria la mitad por cache caliente y el aviso mentiria.
                ayther::VideoProbe cold{};
                check(ayther::video_probe(s, 1200.0, &cold), "se pudo medir el costo");
                check(cold.sampled > 0, "quedaron muestras post-boost");
                check(cold.ms_decode > 0.0, "el decode sale con tiempo");
                check(cold.ms_convert == 0.0,
                      "y la conversion en CPU quedo en CERO (#378: la hace el shader)");
                check(cold.ms_worst >= cold.ms_per_frame(),
                      "el peor frame no puede ser mejor que el promedio");
                check(cold.threads >= 1 && cold.w == s.width() &&
                      cold.h == s.height(),
                      "reporta hilos y dimensiones del clip medido");
                std::printf("       costo medido: %.1f ms/frame (%.1f decode + "
                            "%.1f conversion, peor frame %.1f, %u hilos, %u "
                            "muestras) -> hasta %.0f fps; presupuesto a 60: 16.7\n",
                            cold.ms_per_frame(), cold.ms_decode, cold.ms_convert,
                            cold.ms_worst, cold.threads, cold.sampled,
                            cold.max_fps());
                std::printf("       primera mitad (en turbo) %.1f ms  ·  segunda "
                            "mitad (sostenido) %.1f ms  = la caida del boost\n",
                            cold.ms_burst, cold.ms_per_frame());

                // Frame por frame, byte por byte. Es la afirmacion que el
                // relleno sinttico no puede dar: que el offset del paquete que
                // el streaming calcula es EXACTAMENTE el que el decoder espera.
                uint32_t bad = 0;
                const uint32_t n = s.frame_count() < w.frame_count()
                                 ? s.frame_count() : w.frame_count();
                for (uint32_t i = 0; i < n; ++i) {
                    const ayther::VideoFrameView* fa = s.decode(i);
                    const ayther::VideoFrameView* fb = w.decode(i);
                    // #378: los TRES planos, fila por fila (el stride no es el
                    // ancho). Comparar solo el luma dejaria pasar un croma
                    // corrido, que es justo el defecto que se veria como
                    // «colores raros» y no como «imagen rota».
                    bool eq = fa && fb && fa->w == fb->w && fa->h == fb->h;
                    if (eq) {
                        const ayther::VideoPlane* pa[3] = { &fa->y, &fa->u, &fa->v };
                        const ayther::VideoPlane* pb[3] = { &fb->y, &fb->u, &fb->v };
                        for (int k = 0; k < 3 && eq; ++k)
                            for (uint32_t r = 0; r < pa[k]->h && eq; ++r)
                                eq = std::memcmp(pa[k]->data + size_t(r) * pa[k]->stride,
                                                 pb[k]->data + size_t(r) * pb[k]->stride,
                                                 pa[k]->w) == 0;
                    }
                    if (!eq) {
                        if (bad < 3)
                            std::printf("       frame %u difiere\n", i);
                        ++bad;
                    }
                }
                check(bad == 0, "los " + std::to_string(n) +
                                " frames decodifican identicos por streaming y por buffer");

                // CALIBRACION del probe contra el costo sostenido REAL.
                //
                // El sostenido se mide sobre UN SOLO clip decodificando seguido,
                // que es lo que hace el playback. Medirlo sobre el bucle de
                // comparacion de arriba estaria mal: ese alterna DOS clips con
                // dos frames de 17,7 MB, y esa presion de memoria es un artefacto
                // del test, no de reproducir.
                //
                // Si estos dos numeros se separan, el aviso del Lab miente. Es la
                // unica forma de saberlo: el probe mide un rato y el playback dura
                // minutos.
                double dd0 = 0.0, cc0 = 0.0; uint32_t ff0 = 0;
                s.cost_ms(&dd0, &cc0, &ff0);
                const uint32_t kSust = n < 240 ? n : 240;
                for (uint32_t i = 0; i < kSust; ++i) s.decode((i * 7 + 1) % n);
                double dd1 = 0.0, cc1 = 0.0; uint32_t ff1 = 0;
                s.cost_ms(&dd1, &cc1, &ff1);
                const double sustained = ff1 > ff0
                    ? ((dd1 - dd0) + (cc1 - cc0)) / double(ff1 - ff0) : 0.0;
                std::printf("       costo SOSTENIDO (%u frames de un solo clip): "
                            "%.1f ms/frame  ·  el probe estimo %.1f (%.0f%%)\n",
                            ff1 - ff0, sustained, cold.ms_per_frame(),
                            sustained > 0.0 ? 100.0 * cold.ms_per_frame() / sustained : 0.0);
                // BANDA ANCHA y no una razon estrecha: el sostenido de
                // referencia es el ruidoso de los dos (una sola pasada, sujeta a
                // lo que el resto de la maquina este haciendo). Un assert
                // apretado sobre eso seria un test flaky, que es peor que
                // ninguno. Lo que se afirma es que el probe esta en el mismo
                // orden de magnitud; la razon queda impresa para poder mirarla.
                check(sustained > 0.0 &&
                      cold.ms_per_frame() >= sustained * 0.5 &&
                      cold.ms_per_frame() <= sustained * 2.0,
                      "el probe coincide con el costo sostenido en orden de magnitud");
            }
        }
    } else {
        std::printf("[skip] sin .ivf real — pasar uno all-intra para comparar PIXELES\n");
    }

    fs::remove(ivf_path, ec);
    fs::remove(dir, ec);

    if (g_fail) {
        std::printf("=== %d FALLA(S) ===\n", g_fail);
        return 1;
    }
    std::printf("=== ok ===\n");
    return 0;
}
