// ---------------------------------------------------------------------------
// ayther_video.cpp — ver ayther_video.h
// ---------------------------------------------------------------------------
#include "ayther_video.h"
#include "decode_limits.h"
#include "log.h"

#include "ayther_core_ffi.h"   // : la fuente del pack lee por rango
#include "runtime_options.h"   // AYTHER_VIDEO_THREADS
#include "ayther_file.h"

#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef AYTHER_HAVE_VPX
  #include <vpx/vpx_decoder.h>
  #include <vpx/vp8dx.h>
#endif

namespace ayther {
namespace {

// --- Demuxer IVF -----------------------------------------------------------
// 32 bytes de header + 12 por frame. Es TODO el contenedor; por esto se eligió
// IVF y no WebM, que habría traído libwebm o libavformat detrás. Sale del
// demuxer ya verificado en tools/vpx_smoke/main.cpp, con el buffer en memoria
// en vez de FILE*.

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

/// `off` es uint64: desde  el clip no está en un buffer sino en el archivo,
/// y un video 8K pasa los 4 GB sin esfuerzo.
struct Packet { uint64_t off = 0; uint32_t size = 0; };

/// Un frame absurdo es basura, no un frame gigante. El tope existe para no
/// reservar cientos de MB por un byte torcido.
constexpr uint32_t kMaxPacket = 64u << 20;

/// Bloque del barrido del índice. Ver el comentario de `demux_ivf`.
constexpr size_t kScanBlock = 1u << 20;

/// Índice de los paquetes del IVF, leyendo de una fuente.
///
/// Barre SECUENCIAL y por bloques, no salta de header a header. Los 12 bytes de
/// un header son una lectura ridícula, pero contra el pack cada lectura verifica
/// el TROZO que toca: saltando, un clip de 500 frames pagaría 500
/// verificaciones de trozo para indexarse. Secuencial, cada trozo se verifica
/// una vez y el barrido entero cuesta lo mismo que UNA lectura completa del
/// clip — que es exactamente lo que costaba abrirlo antes de , sólo que
/// ahora no se queda en RAM.
bool demux_ivf(VideoSource& src, uint16_t* w, uint16_t* h, double* fps,
               std::vector<Packet>* out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };

    const uint64_t n = src.size();
    if (n < 32)                          return fail("IVF: archivo mas corto que el header");

    uint8_t hdr[32];
    if (!src.read(0, sizeof(hdr), hdr))  return fail("IVF: no se pudo leer el header");
    if (std::memcmp(hdr, "DKIF", 4) != 0) return fail("IVF: falta la firma DKIF");

    *w = rd16(hdr + 12);
    *h = rd16(hdr + 14);
    if (!*w || !*h)                      return fail("IVF: dimensiones en cero");

    // Time base del header: rate/scale (bytes 16-23). Se ignoraba, y el
    // reproductor asumía que el clip corría a los fps del JUEGO — un clip a 30
    // sobre una toma a 59,92 salía al DOBLE de velocidad y duraba la mitad
    // (reporte 2026-08-07: «debería durar 6 s y dura mucho menos»). 0 = no
    // declarado; el llamador decide el fallback.
    const uint32_t rate = rd32(hdr + 16), scale = rd32(hdr + 20);
    if (fps) *fps = (rate && scale) ? double(rate) / double(scale) : 0.0;

    std::vector<uint8_t> blk;
    uint64_t win_off = 0;      // ventana válida = [win_off, win_off + blk.size())
    uint64_t at      = 32;
    while (at + 12 <= n) {
        // Refrescar la ventana sólo cuando el header no entra entero en ella.
        // Un payload más grande que el bloque hace que `at` salte fuera y la
        // próxima lectura arranque ahí: los bytes salteados no se leen ni se
        // verifican, y está bien — el decode los verificará cuando los use.
        if (at < win_off || at + 12 > win_off + blk.size()) {
            const size_t want = (size_t)std::min<uint64_t>(kScanBlock, n - at);
            blk.resize(want);
            if (!src.read(at, want, blk.data())) break;
            win_off = at;
        }
        const uint32_t sz = rd32(blk.data() + (size_t)(at - win_off));
        at += 12;
        if (sz == 0 || sz > kMaxPacket || at + sz > n) break;
        out->push_back({ at, sz });
        at += sz;
    }
    if (out->empty())                    return fail("IVF: sin frames");
    return true;
}

// --- Índice horneado () -------------------------------------------------
//
// FORMATO, deliberadamente aburrido: 12 bytes de cabecera y 12 por frame.
//
//     0  "AYIX"          firma
//     4  u32 version     1
//     8  u32 frames
//    12  frames × { u64 offset, u32 size }
//
// Little-endian, como el IVF que acompaña. No lleva dimensiones ni time base A
// PROPÓSITO: eso vive en el header del IVF y el sidecar no es autoridad sobre el
// contenido (ver la nota del header). Duplicarlo sólo crearía la posibilidad de
// que discrepen.
//
// Y no lleva hash propio: el sidecar es una entrada del pack, así que el
// `integrity.toml` firmado de  ya lo cubre. Ponerle un hash acá sería una
// segunda garantía, más débil, sobre lo mismo.
constexpr uint32_t kIdxHeader = 12;
constexpr uint32_t kIdxEntry  = 12;
constexpr uint32_t kIdxVersion = 1;

void wr32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
}
void wr64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
uint64_t rd64(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= uint64_t(p[i]) << (8 * i);
    return x;
}

/// Parsea el sidecar y lo comprueba contra `src_size`. Devuelve false —sin
/// ruido— si no es un `.idx` utilizable: un pack viejo no lo trae y eso no es
/// un error, es el caso normal del fallback.
///
/// LA COMPROBACIÓN ES DE PLAUSIBILIDAD, no de contenido: que todo paquete caiga
/// dentro del archivo y que ninguno pase el tope de tamaño. Que el offset tenga
/// realmente un keyframe VP9 lo decide el decoder, y si no lo tiene se pierde
/// ESE frame — el índice dice dónde mirar, no qué hay.
///
/// Sólo lo consume la ruta con VP9. Se compila igual sin decoder —en vez de
/// esconderlo tras el #ifdef— para que el parser no se pudra en silencio.
[[maybe_unused]] bool parse_index(const uint8_t* idx, size_t n,
                                  uint64_t src_size,
                                  std::vector<Packet>* out) {
    if (!idx || n < kIdxHeader) return false;
    if (std::memcmp(idx, "AYIX", 4) != 0) return false;
    if (rd32(idx + 4) != kIdxVersion) return false;
    const uint32_t frames = rd32(idx + 8);
    if (!frames) return false;
    // Tamaño EXACTO: un sidecar truncado o con cola no es «casi bueno».
    if (n != size_t(kIdxHeader) + size_t(frames) * kIdxEntry) return false;

    out->clear();
    out->reserve(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t* e = idx + kIdxHeader + size_t(i) * kIdxEntry;
        const uint64_t off = rd64(e);
        const uint32_t sz  = rd32(e + 8);
        if (sz == 0 || sz > kMaxPacket) return false;
        if (off < 32 || off > src_size || sz > src_size - off) return false;
        out->push_back({ off, sz });
    }
    return true;
}

// --- Fuentes ---------------------------------------------------------------

/// Un .ivf ya residente en RAM. Es lo que sostiene la sobrecarga por buffer de
/// `open`: el clip se queda con SU copia, así el llamador puede soltar la suya.
struct MemSource final : VideoSource {
    std::vector<uint8_t> bytes;
    uint64_t size() const override { return bytes.size(); }
    bool read(uint64_t off, size_t len, uint8_t* dst) override {
        if (off > bytes.size() || len > bytes.size() - off) return false;
        std::memcpy(dst, bytes.data() + off, len);
        return true;
    }
    /// La única fuente que RESIDE. Que lo declare es lo que hace medible la
    /// diferencia con el streaming.
    uint64_t resident_bytes() const override { return bytes.capacity(); }
};

/// Fuente sobre una entrada del pack. Ver `video_source_from_pack`.
struct PackSource final : VideoSource {
    AyArchive*  pack = nullptr;   // NO es dueña: el pack tiene que sobrevivirla
    std::string path;
    uint64_t    n = 0;
    uint64_t size() const override { return n; }
    bool read(uint64_t off, size_t len, uint8_t* dst) override {
        if (!pack || off > n || len > n - off) return false;
        // Una lectura corta es un FALLO: el demuxer distingue «no llegué» de
        // «llegué al final» por el tamaño total, no por lo que devolvió el read.
        return ayther_pack_read_range(pack, path.c_str(), off, dst, len)
               == (int64_t)len;
    }
};

struct FileSource final : VideoSource {
    std::FILE* f = nullptr;
    uint64_t   n = 0;
    ~FileSource() override { if (f) std::fclose(f); }
    uint64_t size() const override { return n; }
    bool read(uint64_t off, size_t len, uint8_t* dst) override {
        if (!f || off > n || len > n - off) return false;
        // _fseeki64: un fseek de long se queda corto en 32 bits y un video de
        // 8K pasa los 2 GB.
#ifdef _WIN32
        if (_fseeki64(f, (long long)off, SEEK_SET) != 0) return false;
#else
        if (std::fseek(f, (long)off, SEEK_SET) != 0) return false;
#endif
        return std::fread(dst, 1, len, f) == len;
    }
};

}  // namespace

std::unique_ptr<VideoSource> video_source_from_file(const std::string& path) {
    auto s = std::make_unique<FileSource>();
    s->f = ayther::file_open(path.c_str(), "rb");
    if (!s->f) return nullptr;
#ifdef _WIN32
    if (_fseeki64(s->f, 0, SEEK_END) != 0) return nullptr;
    const long long end = _ftelli64(s->f);
#else
    if (std::fseek(s->f, 0, SEEK_END) != 0) return nullptr;
    const long end = std::ftell(s->f);
#endif
    if (end <= 0) return nullptr;
    s->n = (uint64_t)end;
    return s;
}

std::unique_ptr<VideoSource> video_source_from_pack(void* pack,
                                                   const std::string& logical_path) {
    AyArchive* p = static_cast<AyArchive*>(pack);
    if (!p) return nullptr;
    // Sin streaming no se devuelve una fuente a medias: el llamador tiene que
    // poder caer al camino de lectura entera, y para eso necesita un nullptr.
    if (!ayther_pack_entry_streamable(p, logical_path.c_str())) return nullptr;
    const int64_t n = ayther_pack_file_size(p, logical_path.c_str());
    if (n <= 0) return nullptr;

    auto s = std::make_unique<PackSource>();
    s->pack = p;
    s->path = logical_path;
    s->n    = (uint64_t)n;
    return s;
}

namespace {

#ifdef AYTHER_HAVE_VPX

#endif  // AYTHER_HAVE_VPX

}  // namespace

// ---------------------------------------------------------------------------

struct VideoClip::Impl {
    /// : la fuente, no los bytes. Con  encima, la residencia del clip es
    /// SÓLO el paquete en curso y el índice — ni el archivo ni el frame.
    std::unique_ptr<VideoSource> src;
    std::vector<Packet>  packets;
    uint16_t             w = 0, h = 0;
    double               fps = 0.0;   ///< del time base del IVF (0 = no declarado)

    std::vector<uint8_t> pkt;       ///< paquete en curso; crece al mayor pedido
    /// : ya NO hay buffer de salida. La vista apunta a los planos del propio
    /// decoder, así que la residencia del clip es el paquete y nada más — de
    /// 17,7 MB a ~200 KB en el clip real.
    VideoFrameView       view{};
    bool                 have  = false;   ///< `view` tiene contenido válido
    uint64_t             seq   = 0;

    // Desglose del costo (ver cost_ms). Barato —dos relojes por frame— y es el
    // único número que dice si el arreglo es YUV-en-shader o menos resolución.
    double   dec_ms = 0.0, cvt_ms = 0.0;
    uint32_t decoded = 0;
    /// Hilos con los que se abrió el decoder. Se reporta porque «el video es
    /// lento» sin saber cuántos hilos hay no dice nada.
    unsigned threads = 1;

#ifdef AYTHER_HAVE_VPX
    vpx_codec_ctx_t ctx{};
    bool            ctx_ok = false;
#endif

    ~Impl() {
#ifdef AYTHER_HAVE_VPX
        if (ctx_ok) vpx_codec_destroy(&ctx);
#endif
    }
};

VideoClip::VideoClip()  = default;
VideoClip::~VideoClip() = default;

bool VideoClip::available() {
#ifdef AYTHER_HAVE_VPX
    return true;
#else
    return false;
#endif
}

bool VideoClip::open(const uint8_t* ivf, size_t n, std::string* err) {
    if (!ivf) { if (err) *err = "IVF: buffer nulo"; return false; }
    auto mem = std::make_unique<MemSource>();
    mem->bytes.assign(ivf, ivf + n);
    return open(std::move(mem), err);
}

bool VideoClip::open(std::unique_ptr<VideoSource> src, std::string* err) {
    return open(std::move(src), nullptr, 0, err);
}

bool VideoClip::open(std::unique_ptr<VideoSource> src, const uint8_t* idx,
                     size_t idx_n, std::string* err) {
#ifndef AYTHER_HAVE_VPX
    (void)src; (void)idx; (void)idx_n;
    if (err) *err = "el engine se construyo sin libvpx (ver tools/build_libvpx.ps1)";
    return false;
#else
    if (!src) { if (err) *err = "IVF: fuente nula"; return false; }
    auto im = std::make_unique<Impl>();
    // : con sidecar utilizable se saltea el barrido entero. El header del
    // IVF se lee IGUAL —32 bytes, un solo trozo— porque de ahí salen las
    // dimensiones y el time base, que el índice deliberadamente no lleva.
    bool indexed = false;
    if (parse_index(idx, idx_n, src->size(), &im->packets)) {
        uint8_t hdr[32];
        if (src->read(0, sizeof(hdr), hdr) && std::memcmp(hdr, "DKIF", 4) == 0) {
            im->w = rd16(hdr + 12);
            im->h = rd16(hdr + 14);
            const uint32_t rate = rd32(hdr + 16), scale = rd32(hdr + 20);
            im->fps = (rate && scale) ? double(rate) / double(scale) : 0.0;
            indexed = im->w && im->h;
        }
        if (!indexed) im->packets.clear();
    }
    if (!indexed && !demux_ivf(*src, &im->w, &im->h, &im->fps, &im->packets, err))
        return false;

    // The IVF header carries 16-bit dimensions, so a handful of bytes can ask
    // for a 65535x65535 surface. The decoder allocates several frames of it, so
    // the declared size is checked before libvpx is handed the configuration.
    if (!ayther::limits::video_dimensions_ok(im->w, im->h)) {
        if (err) *err = "video dimensions exceed the decode ceiling";
        ayther::log::write(ayther::log::Severity::Error,
            "video", "dimensions_refused",
            "refusing a %ux%u stream: the ceiling is %lld pixels and %lld per side",
            im->w, im->h,
            static_cast<long long>(ayther::limits::kMaxVideoPixels),
            static_cast<long long>(ayther::limits::kMaxVideoDimension));
        return false;
    }
    im->src = std::move(src);

    vpx_codec_iface_t* iface = vpx_codec_vp9_dx();
    if (!iface) { if (err) *err = "libvpx sin decoder VP9"; return false; }

    // HILOS. Se pasaba `nullptr` como cfg, que es UN hilo — y a resolución de
    // tier el decode es el gasto que manda. Medido en «Tyris' History»
    // (2304×2016, 8 hilos): decode+conversión 35,5 ms por frame con un hilo
    // contra 20,3 con ocho (×1,75 punta a punta; el decode SOLO baja de ~23 a
    // 7,8, o sea ×2,9). El presupuesto son 16,7 a 60 fps: con un hilo el clip
    // corría a 28 fps, con ocho a 49. No era el formato ni el pack — era no
    // pedirle al decoder los núcleos que hay.
    //
    // `row_mt` reparte POR FILAS dentro del frame. Es lo que sirve acá y el
    // frame threading no: todos los frames son keyframes y se decodifican de a
    // uno, por pedido, así que no hay frames en vuelo entre los que paralelizar.
    // `loop_filter_opt` deja que cada hilo filtre sin esperar la sincronización
    // de todos.
    //
    // Tope de 8: más hilos no compran nada en un frame de este tamaño y cada uno
    // cuesta buffers. AYTHER_VIDEO_THREADS fuerza el valor —el oráculo lo usa
    // para comparar 1 hilo contra N y verificar que los píxeles no cambian.
    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 1;
    if (threads > 8) threads = 8;
    if (const uint32_t forced = RuntimeOptions::process().video_threads();
        forced != RuntimeOptions::kVideoThreadsAuto) {
        threads = forced;
    }

    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = threads;
    cfg.w       = im->w;
    cfg.h       = im->h;

    const vpx_codec_err_t e = vpx_codec_dec_init(&im->ctx, iface, &cfg, 0);
    if (e != VPX_CODEC_OK) {
        if (err) *err = std::string("vpx_codec_dec_init: ") + vpx_codec_err_to_string(e);
        return false;
    }
    im->ctx_ok  = true;
    im->threads = threads;
    // Los dos son de VP9 y no fallan si el build no los tiene: se ignora el
    // error a propósito, porque un decoder sin row-mt igual decodifica bien.
    vpx_codec_control(&im->ctx, VP9D_SET_ROW_MT,          1);
    vpx_codec_control(&im->ctx, VP9D_SET_LOOP_FILTER_OPT, 1);

    impl_ = std::move(im);
    return true;
#endif
}

bool     VideoClip::is_open()     const { return impl_ != nullptr; }
uint32_t VideoClip::frame_count() const { return impl_ ? uint32_t(impl_->packets.size()) : 0; }
uint32_t VideoClip::width()       const { return impl_ ? impl_->w : 0; }
uint32_t VideoClip::height()      const { return impl_ ? impl_->h : 0; }
double   VideoClip::fps()         const { return impl_ ? impl_->fps : 0.0; }

const VideoFrameView* VideoClip::decode(uint32_t index) {
#ifndef AYTHER_HAVE_VPX
    (void)index;
    return nullptr;
#else
    if (!impl_ || impl_->packets.empty()) return nullptr;
    Impl& im = *impl_;

    if (index >= im.packets.size()) return nullptr;

    // Idempotencia (ver el header): mismo índice, mismo resultado, y `seq` NO
    // se mueve. Es lo que hace que un re-produce del mismo frame de juego no
    // adelante el video.
    if (im.have && im.view.index == index) return &im.view;

    // Todos los frames son keyframes (lo garantiza validate() en el bake), así
    // que decodificar el N es UN paquete: no hay que caminar el GOP. Y como es
    // UN paquete, es también lo único que hay que traer de la fuente ().
    const Packet& pk = im.packets[index];
    if (im.pkt.size() < pk.size) im.pkt.resize(pk.size);
    if (!im.src || !im.src->read(pk.off, pk.size, im.pkt.data())) {
        // Un trozo que no verifica llega hasta acá: se pierde el frame, no el
        // clip. `have` queda como estaba, así que la pantalla conserva el
        // último frame bueno en vez de parpadear a negro.
        ayther::log::write(ayther::log::Severity::Error,
            "video", "frame_pudo_leer_paquete",
            "frame %u: no se pudo leer el paquete",
            index);
        return nullptr;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const vpx_codec_err_t e =
        vpx_codec_decode(&im.ctx, im.pkt.data(), pk.size, nullptr, 0);
    if (e != VPX_CODEC_OK) return nullptr;

    vpx_codec_iter_t it = nullptr;
    vpx_image_t* img = vpx_codec_get_frame(&im.ctx, &it);
    if (!img) return nullptr;
    const auto t1 = std::chrono::steady_clock::now();

    // Un clip cuyas dims cambian a mitad no está soportado: la textura del
    // renderer se crea una vez con las dims del clip. Se rechaza el frame en
    // vez de escribir fuera del buffer.
    if (img->d_w != im.w || img->d_h != im.h) return nullptr;

    {
        using msd = std::chrono::duration<double, std::milli>;
        im.dec_ms += msd(t1 - t0).count();
        // cvt_ms queda en CERO desde  — no hay conversión en CPU. Se sigue
        // sumando (cero) a propósito: es la medida de que sigue en cero.
        ++im.decoded;
    }

    // Los planos apuntan al frame del DECODER, sin copia. Son válidos hasta el
    // próximo vpx_codec_decode, y la idempotencia de decode() garantiza que no
    // haya ninguno entre que se devuelve esta vista y que el renderer la sube.
    // El stride NO es `w`: libvpx alinea las filas.
    im.view.y = { img->planes[VPX_PLANE_Y], uint32_t(img->stride[VPX_PLANE_Y]),
                  img->d_w, img->d_h };
    im.view.u = { img->planes[VPX_PLANE_U], uint32_t(img->stride[VPX_PLANE_U]),
                  (img->d_w + 1) / 2, (img->d_h + 1) / 2 };
    im.view.v = { img->planes[VPX_PLANE_V], uint32_t(img->stride[VPX_PLANE_V]),
                  (img->d_w + 1) / 2, (img->d_h + 1) / 2 };
    im.view.w     = im.w;
    im.view.h     = im.h;
    im.view.index = index;
    im.view.seq   = ++im.seq;
    im.have       = true;
    return &im.view;
#endif
}

uint64_t VideoClip::resident_bytes() const {
    if (!impl_) return 0;
    const Impl& im = *impl_;
    // : sin buffer de salida. Lo que queda es el índice, el paquete en curso
    // y lo que retenga la fuente. Los planos del frame los tiene el decoder en
    // su propio pool y no se cuentan acá: no son del clip.
    return uint64_t(im.packets.capacity() * sizeof(Packet))
         + im.pkt.capacity()
         + (im.src ? im.src->resident_bytes() : 0);
}

unsigned VideoClip::threads() const { return impl_ ? impl_->threads : 0u; }

bool video_probe(VideoClip& clip, double measure_ms, VideoProbe* out) {
    if (!out) return false;
    *out = VideoProbe{};
    const uint32_t n = clip.frame_count();
    if (n == 0) return false;
    if (measure_ms < 1.0) measure_ms = 1.0;

    out->w       = clip.width();
    out->h       = clip.height();
    out->threads = clip.threads();

    // Calentar. Con UN solo frame no hay dónde, y devolver false ahí sería
    // reportar «no se pudo medir» sobre un clip perfectamente medible.
    if (n > 1 && !clip.decode(n / 2)) return false;

    // Tope de frames: cota para que un clip absurdamente rápido no gire eterno,
    // y para que uno absurdamente lento corte igual.
    constexpr uint32_t kMaxSamples = 2048;

    struct Sample { double ms; };
    std::vector<Sample> ms;
    ms.reserve(256);

    double d0 = 0.0, c0 = 0.0;
    uint32_t f0 = 0;
    clip.cost_ms(&d0, &c0, &f0);

    // Consecutivos arrancando por el MEDIO del clip y dando la vuelta.
    const uint32_t first = n / 2;
    const auto t_start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kMaxSamples; ++i) {
        if (!clip.decode(uint32_t((uint64_t(first) + i) % n))) return false;

        double d1 = 0.0, c1 = 0.0;
        uint32_t f1 = 0;
        clip.cost_ms(&d1, &c1, &f1);
        if (f1 > f0)   // decode() es idempotente: sin re-decode no hay muestra
            ms.push_back({ ((d1 - d0) + (c1 - c0)) / double(f1 - f0) });
        d0 = d1; c0 = c1; f0 = f1;

        using msd = std::chrono::duration<double, std::milli>;
        if (msd(std::chrono::steady_clock::now() - t_start).count() >= measure_ms)
            break;
    }
    if (ms.empty()) return false;

    // SEGUNDA MITAD: post-boost. La primera queda en `ms_burst` para que la
    // caída sea visible (ver el comentario del header).
    const size_t half = ms.size() / 2;
    const size_t from = ms.size() > 1 ? half : 0;

    double sum_late = 0.0, worst = 0.0;
    for (size_t i = from; i < ms.size(); ++i) {
        sum_late += ms[i].ms;
        if (ms[i].ms > worst) worst = ms[i].ms;
    }
    const size_t late = ms.size() - from;
    double sum_early = 0.0;
    for (size_t i = 0; i < from; ++i) sum_early += ms[i].ms;

    out->sampled  = uint32_t(late);
    out->ms_worst = worst;
    out->ms_burst = from > 0 ? sum_early / double(from) : sum_late / double(late);

    // El desglose decode/convert se reparte con la proporción del acumulado del
    // clip: `cost_ms` los separa, pero por muestra sólo se guardó el total (dos
    // relojes por frame ya son el costo que este probe puede pagar sin
    // distorsionar lo que mide).
    double dt = 0.0, ct = 0.0;
    uint32_t ft = 0;
    clip.cost_ms(&dt, &ct, &ft);
    const double total = dt + ct;
    const double mean  = sum_late / double(late);
    out->ms_decode  = total > 0.0 ? mean * (dt / total) : mean;
    out->ms_convert = total > 0.0 ? mean * (ct / total) : 0.0;
    return true;
}

void VideoClip::cost_ms(double* decode, double* convert, uint32_t* frames) const {
    if (decode)  *decode  = impl_ ? impl_->dec_ms  : 0.0;
    if (convert) *convert = impl_ ? impl_->cvt_ms  : 0.0;
    if (frames)  *frames  = impl_ ? impl_->decoded : 0u;
}

bool VideoClip::validate(const uint8_t* ivf, size_t n,
                         VideoInfo* out, std::string* err) {
    if (!ivf) { if (err) *err = "IVF: buffer nulo"; return false; }
    MemSource mem;
    mem.bytes.assign(ivf, ivf + n);
    return validate(mem, out, err);
}

// ---------------------------------------------------------------------------
// Índice horneado — la cara pública ()
// ---------------------------------------------------------------------------
bool video_index_build(VideoSource& src, std::vector<uint8_t>* out,
                       std::string* err) {
    if (!out) { if (err) *err = "índice: destino nulo"; return false; }
    uint16_t w = 0, h = 0;
    double   fps = 0.0;
    std::vector<Packet> packets;
    if (!demux_ivf(src, &w, &h, &fps, &packets, err)) return false;
    if (packets.size() > 0xFFFFFFFFull) {
        if (err) *err = "índice: demasiados frames";
        return false;
    }
    out->clear();
    out->reserve(kIdxHeader + packets.size() * kIdxEntry);
    out->insert(out->end(), { 'A', 'Y', 'I', 'X' });
    wr32(*out, kIdxVersion);
    wr32(*out, uint32_t(packets.size()));
    for (const Packet& p : packets) { wr64(*out, p.off); wr32(*out, p.size); }
    return true;
}

uint32_t video_index_frames(const uint8_t* idx, size_t n) {
    if (!idx || n < kIdxHeader) return 0;
    if (std::memcmp(idx, "AYIX", 4) != 0) return 0;
    if (rd32(idx + 4) != kIdxVersion) return 0;
    const uint32_t frames = rd32(idx + 8);
    if (!frames) return 0;
    return n == size_t(kIdxHeader) + size_t(frames) * kIdxEntry ? frames : 0;
}

bool VideoClip::validate(VideoSource& src, VideoInfo* out, std::string* err) {
    VideoInfo info{};

    uint16_t w = 0, h = 0;
    double   fps = 0.0;
    std::vector<Packet> packets;
    if (!demux_ivf(src, &w, &h, &fps, &packets, err)) return false;
    info.frames = uint32_t(packets.size());
    info.w = w;
    info.h = h;
    info.fps = fps;

#ifndef AYTHER_HAVE_VPX
    if (err) *err = "el engine se construyo sin libvpx: no se puede validar el video";
    if (out) *out = info;
    return false;
#else
    // Todos keyframes. Sin esto, decodificar el frame N exigiría caminar desde
    // el keyframe anterior y la promesa de «saltar al medio cae en el frame
    // correcto» costaría un GOP entero por salto.
    //
    // Se trae UN paquete a la vez: el pico de RAM es el frame más grande, no el
    // clip. Es lo que permite validar un video de 8K en el bake sin reservar un
    // giga () — antes el llamador tenía que leer el archivo entero para
    // poder pasarlo por acá, y ése era el tope de 32 MB en la práctica.
    std::vector<uint8_t> pkt;
    for (size_t i = 0; i < packets.size(); ++i) {
        if (pkt.size() < packets[i].size) pkt.resize(packets[i].size);
        if (!src.read(packets[i].off, packets[i].size, pkt.data())) {
            if (err) *err = "frame " + std::to_string(i) + ": no se pudo leer";
            if (out) *out = info;
            return false;
        }
        vpx_codec_stream_info_t si{};
        si.sz = sizeof(si);
        const vpx_codec_err_t e = vpx_codec_peek_stream_info(
            vpx_codec_vp9_dx(), pkt.data(), packets[i].size, &si);
        if (e != VPX_CODEC_OK) {
            if (err) *err = "frame " + std::to_string(i) + ": no es VP9 legible";
            if (out) *out = info;
            return false;
        }
        if (!si.is_kf) {
            if (err)
                *err = "frame " + std::to_string(i) + " no es keyframe. El video "
                       "tiene que ser all-intra para que saltar al medio sea "
                       "barato: re-codificar con -g 1 (ffmpeg: -c:v libvpx-vp9 "
                       "-g 1 -f ivf).";
            if (out) *out = info;
            return false;
        }
    }
    info.all_keyframes = true;
    if (out) *out = info;
    return true;
#endif
}

}  // namespace ayther
