#pragma once
// ---------------------------------------------------------------------------
// ayther_video.h — decoder de video para la Cinemática ().
//
// QUÉ ES: un clip VP9 en contenedor IVF, decodificado a BGRA8 listo para subir
// a una textura. No sabe nada de Vulkan ni de la sesión: se le pide un índice
// de frame y devuelve píxeles.
//
// POR QUÉ VP9/IVF Y NO FFmpeg — no es gusto, es la frontera GPL del proyecto.
// `ayther_engine` es una lib ESTÁTICA; el núcleo de FFmpeg es LGPL-2.1+, así
// que linkearlo obligaría a distribución dinámica o a entregar objetos
// relinkeables, y cualquier componente --enable-gpl lo volvería GPL. libvpx es
// BSD-3 + patent grant. IVF son 32 bytes de header y 12 por frame, o sea que el
// demuxer entra en este archivo y no arrastra libavformat. El ENCODER sigue
// siendo un ffmpeg.exe EXTERNO del PATH (lab/src/app/ffmpeg_pipe.h): proceso
// aparte, sin linkeo, sin pregunta de licencia.
//
// POR QUÉ EL HEADER NO INCLUYE vpx: si `vpx/vpx_decoder.h` entrara acá, el
// include dir de libvpx tendría que ser PUBLIC en el CMake del engine y el Lab,
// el runtime y los tools pasarían a depender de una librería OPCIONAL. Pimpl.
//
// SIN libvpx (AYTHER_HAVE_VPX apagado) esto compila igual: open() devuelve
// false con motivo y validate() rechaza. Un colaborador sin libvpx no queda
// bloqueado, y el bake nunca hornea un video sin validar.
//
// STREAMING (): el clip NO reside en RAM. Lee de una `VideoSource` —el pack
// por rango o un archivo— y se queda sólo con el índice de paquetes, el paquete
// en curso y el frame convertido. Antes copiaba el .ivf entero, y ése era el
// motivo real del tope de 32 MB por video del bake: no el formato, sino que
// `ayther_pack_read` es todo-o-nada. Retirado el tope, un clip de 8K ocupa lo
// mismo que uno de 3 s.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ayther {

/// De dónde salen los bytes del clip. Existe para que el reproductor no
/// dependa de tener el archivo en memoria: el pack la implementa con
/// `ayther_pack_read_range` (verificando por trozo contra el índice firmado) y
/// el disco con un FILE*.
///
/// El clip la conserva durante toda su vida, así que la fuente tiene que
/// sobrevivirlo. En el pack eso significa que reabrir el pack INVALIDA los
/// clips abiertos — la sesión limpia su cache en `set_pack`.
struct VideoSource {
    virtual ~VideoSource() = default;
    /// Tamaño total del .ivf en bytes.
    virtual uint64_t size() const = 0;
    /// Copia EXACTAMENTE `len` bytes desde `off` a `dst`. false = no se pudo
    /// (rango fuera, IO, o un trozo que no verifica): una lectura corta es un
    /// fallo, no un resultado parcial — el demuxer necesita esa garantía para
    /// no confundir «se cortó» con «llegué al final».
    virtual bool read(uint64_t off, size_t len, uint8_t* dst) = 0;
    /// Cuánta RAM se queda la fuente. Cero para las que leen del disco o del
    /// pack; el tamaño del archivo para la que lo tiene en memoria. Está acá
    /// para que «esto es streaming» sea una MEDICIÓN y no una afirmación —
    /// mismo criterio que `VideoClip::cost_ms`.
    virtual uint64_t resident_bytes() const { return 0; }
};

/// Fuente sobre un archivo del disco. La usan la autoría (el Lab trabaja contra
/// el proyecto, sin pack) y el bake al validar.
std::unique_ptr<VideoSource> video_source_from_file(const std::string& path);

/// Fuente sobre una entrada del pack, leída POR RANGO (). `pack` es un
/// `AyArchive*` (se toma como void* para no arrastrar el FFI del core a este
/// header, que lo incluye medio mundo).
///
/// Cada lectura verifica los trozos que toca contra el índice firmado, así que
/// el streaming NO afloja la garantía de : cambia la unidad de verificación,
/// de la entrada al trozo.
///
/// NO es dueña del pack. El pack tiene que sobrevivir al clip — la sesión lo
/// garantiza limpiando su cache de videos antes de cerrarlo (`set_pack`).
/// Devuelve nullptr si la entrada no es direccionable por rango.
std::unique_ptr<VideoSource> video_source_from_pack(void* pack,
                                                    const std::string& logical_path);

// ---------------------------------------------------------------------------
// ÍNDICE DE PAQUETES HORNEADO () — el sidecar `<nombre>.ivf.idx`
//
// QUÉ RESUELVE. Desde  el clip no reside: se lee el paquete del frame que
// toca y nada más. Pero para saber DÓNDE está cada paquete hay que barrer el
// IVF entero al abrirlo — una pasada secuencial que cuesta lo mismo que leerlo
// completo (medido 810 MB/s con SHA-256 de por medio). No es una regresión: es
// el costo que  no bajó. En las Cinemáticas de hoy son 60-83 ms y se tapan
// con prewarm; en un 8K de 30 s serían ~2,4 s de freeze justo al arrancar la
// escena que el artista quiere que se vea bien.
//
// POR QUÉ UN ASSET DEL PACK Y NO UN FORMATO NUEVO. El sidecar entra al pack como
// cualquier otra entrada, así que queda cubierto por `integrity.toml` y por la
// firma SIN maquinaria nueva, y se verifica al leerlo igual que todo lo demás.
//
// EL ÍNDICE NO ES AUTORIDAD SOBRE EL CONTENIDO. Dice DÓNDE mirar, igual que el
// `data_start` del directorio central del ZIP: si señala un offset que no tiene
// un keyframe VP9, el decode de ese frame falla y se pierde ESE frame — no se
// corrompe nada. Los hashes por trozo siguen siendo los que garantizan los
// bytes. Por eso las dimensiones y el time base se siguen leyendo del header
// del IVF y NO del sidecar: un `.idx` que mintiera sobre el tamaño del cuadro
// sería una autoridad que no le corresponde.
//
// EL FALLBACK TIENE QUE EXISTIR. Packs anteriores no lo traen, y en el Lab el
// asset del paso es una ruta absoluta a un `.ivf` suelto sin pack ninguno. Sin
// sidecar —o con uno que no cuadre con la fuente— se barre, como siempre.
// ---------------------------------------------------------------------------

/// Sufijo del sidecar: «clip.ivf» → «clip.ivf.idx».
///
///  EXCEPTA ESTA RELACIÓN de su propia regla, y conviene dejar escrito por
/// qué. El contrato dice que ninguna relación del pack se deriva del nombre: el
/// TOML nombra explícitamente lo que referencia. Acá no se puede, y no es una
/// deuda.
///
/// El índice es POR TIER: cada `assets/tiers/<t>/<id>` tiene su propio `.idx`,
/// porque cada tier es un archivo distinto con sus paquetes en otras
/// posiciones. Un campo en el TOML nombraría UNO, y el TOML no sabe —ni debe
/// saber— con qué tier se va a abrir el pack: eso lo decide el display de la
/// máquina que lo corre.
///
/// El sufijo, en cambio, viaja PEGADO al nombre lógico y pasa por la misma
/// resolución por tier que el clip: pedir `<id>.idx` devuelve el índice del
/// tier activo, sin que nadie tenga que enumerarlos. La derivación es lo que
/// hace correcta a la relación, no lo que la debilita.
inline std::string video_index_path(const std::string& ivf_path) {
    return ivf_path + ".idx";
}

/// Serializa el índice de paquetes de `src` (barre el IVF UNA vez). Lo usa el
/// bake, que ya paga ese barrido para exigir all-keyframes.
/// false = la fuente no es un IVF utilizable (motivo en *err).
bool video_index_build(VideoSource& src, std::vector<uint8_t>* out,
                       std::string* err);

/// Cuántos frames declara un `.idx` bien formado. 0 = no lo es. Existe para que
/// el bake pueda decir cuántos horneó sin re-parsear a mano.
uint32_t video_index_frames(const uint8_t* idx, size_t n);


/// Un plano de luma o croma tal como lo entrega el decoder. `stride` NO es `w`:
/// libvpx alinea las filas, y leerlo como si fuera compacto corre la imagen.
struct VideoPlane {
    const uint8_t* data   = nullptr;
    uint32_t       stride = 0;
    uint32_t       w      = 0;
    uint32_t       h      = 0;
};

/// Un frame decodificado, en I420 y SIN COPIA ().
///
/// Los planos apuntan al frame del propio decoder y viven hasta el próximo
/// decode() — el consumidor los sube y los olvida, no los guarda. Antes acá
/// había un BGRA8 convertido en CPU, y era el gasto mayor de todo el
/// reproductor: medido 12,5 de los 20,3 ms por frame en 2304×2016 (62%), más
/// 17,7 MB de buffer que ERAN la residencia entera del clip. La conversión la
/// hace ahora el fragment shader, que además sube 1,5 bytes por píxel en vez
/// de 4.
struct VideoFrameView {
    VideoPlane  y, u, v;           ///< I420: u y v a la mitad en los dos ejes
    uint32_t    w     = 0;
    uint32_t    h     = 0;
    uint32_t    index = 0;         ///< frame del clip que se decodificó

    /// Cambia SÓLO cuando el contenido cambió. El renderer lo usa para saltarse
    /// la re-subida cuando el video no avanzó (pausa, o un re-render de UI
    /// sobre la misma FrameView): sin esto se paga el memcpy completo por cada
    /// frame de interfaz estando detenido.
    uint64_t    seq   = 0;
};

/// LA ESPECIFICACIÓN de la conversión que hace el shader (), en C++.
///
/// BT.601 limited range —lo que produce ffmpeg por defecto para SD— con la
/// aritmética ENTERA que hacía `i420_to_bgra` en CPU antes de que la conversión
/// se fuera al fragment shader. Existe para que el oráculo pueda comparar el
/// resultado de la GPU contra un número calculado acá: sin una referencia
/// ejecutable, «el shader convierte bien» no se puede afirmar.
///
/// Los coeficientes NO se tocan sin re-medir shaders/video.frag contra esto.
inline void video_i420_to_bgra_px(int Y, int U, int V, uint8_t out_bgra[4]) {
    const int C = Y - 16, D = U - 128, E = V - 128;
    const int r = (298 * C + 409 * E + 128) >> 8;
    const int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
    const int b = (298 * C + 516 * D + 128) >> 8;
    auto clamp8 = [](int v) -> uint8_t {
        return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    out_bgra[0] = clamp8(b);
    out_bgra[1] = clamp8(g);
    out_bgra[2] = clamp8(r);
    out_bgra[3] = 255;   // el video es opaco: cubre la pantalla entera
}

/// Resultado de validar un .ivf sin abrirlo del todo. Lo usa el bake.
struct VideoInfo {
    uint32_t frames = 0;
    uint32_t w      = 0;
    uint32_t h      = 0;
    /// Time base declarado en el header IVF (0 = ausente). El reproductor lo
    /// NECESITA: sin él asumía que el clip corre a los fps del juego y un clip
    /// a 30 sobre una toma a 59,92 salía al doble de velocidad.
    double   fps    = 0.0;
    bool     all_keyframes = false;
};

/// Costo MEDIDO de reproducir un clip en ESTA máquina ().
///
/// Existe porque un clip puede ser perfectamente válido —all-keyframes,
/// decodificable— y no entrar en presupuesto: medido 20,3 ms por frame en
/// 2304×2016 contra 16,7 a 60 fps, o sea ~49 fps. Y no se puede tabular de
/// antemano, porque depende de los núcleos de la máquina; entonces no se
/// estima, se mide.
struct VideoProbe {
    /// Promedio por frame de la SEGUNDA MITAD de la medición — ver `ms_burst`.
    double   ms_decode  = 0.0;
    double   ms_convert = 0.0;   ///< I420→BGRA (lo elimina subir 3 planos R8)
    /// El frame MÁS CARO de la segunda mitad. Va aparte del promedio porque un
    /// tirón lo produce el frame malo, no la media: un clip que promedia 15 ms y
    /// pica a 40 se ve peor que uno parejo de 18.
    double   ms_worst   = 0.0;
    /// Promedio de la PRIMERA mitad. Existe como GUARDIA de la propia medición:
    /// si el turbo del CPU inflara el arranque, `ms_burst` mucho menor que
    /// `ms_per_frame()` lo delataría, y nadie podría «mejorar» el número
    /// acortando la medición sin que se vea. En la máquina donde se calibró no
    /// hubo caída medible (20,1 contra 20,1 ms), pero la garantía no puede
    /// depender de eso: en una laptop que throttlea, sí la va a haber.
    double   ms_burst   = 0.0;
    uint32_t sampled    = 0;     ///< frames de la segunda mitad (0 = no se midió)
    uint32_t w = 0, h = 0;
    unsigned threads    = 0;

    double ms_per_frame() const { return ms_decode + ms_convert; }
    double max_fps()     const {
        return ms_per_frame() > 0.0 ? 1000.0 / ms_per_frame() : 0.0;
    }
};

/// @brief Move-disabled owner for a streamed or memory-backed VP9 clip.
///
/// Opening transfers ownership of the VideoSource. Decoded frame views are
/// borrowed from the clip and are invalidated by the next successful decode,
/// reopen, or destruction. The class is not thread-safe.
///
/// Input dimensions, packet ranges, and sidecar indexes are untrusted and must
/// pass bounds validation before allocation or decoding.
class VideoClip {
public:
    VideoClip();
    ~VideoClip();
    VideoClip(const VideoClip&)            = delete;
    VideoClip& operator=(const VideoClip&) = delete;

    /// Demuxea el IVF sobre `src` y abre el decoder VP9. El clip se queda con
    /// la fuente: nada del contenido se copia a RAM salvo el índice de paquetes.
    ///
    /// El barrido del índice lee el archivo SECUENCIAL y por bloques, no
    /// saltando de header a header. Con la fuente del pack, pedir 12 bytes
    /// cuesta verificar un trozo entero, así que saltar pagaría ese trozo una
    /// vez POR FRAME; secuencial cada trozo se verifica una sola vez.
    bool open(std::unique_ptr<VideoSource> src, std::string* err);

    /// Igual, con el ÍNDICE HORNEADO (): si `idx` es un sidecar bien formado
    /// y COHERENTE con la fuente, se salta el barrido entero. Si no lo es —o si
    /// `idx` es nulo— se barre, y sin decir nada: un pack viejo no trae sidecar
    /// y eso no es un error.
    ///
    /// «Coherente» se comprueba: todo paquete tiene que caer DENTRO del archivo
    /// y ninguno puede pasarse del tope de tamaño. Es una validacion de
    /// PLAUSIBILIDAD, no de contenido — que un offset apunte a un keyframe de
    /// verdad lo decide el decoder, y si no lo es se pierde ese frame y nada mas
    /// (ver la nota de arriba sobre por que el indice no es autoridad).
    bool open(std::unique_ptr<VideoSource> src, const uint8_t* idx, size_t idx_n,
              std::string* err);

    /// Igual, sobre un buffer que ya está en RAM. COPIA los bytes (el buffer
    /// del llamador no tiene por qué sobrevivir al clip). Es el camino de los
    /// clips chicos y de todo lo que ya tiene el archivo leído; para un video
    /// grande, usar la sobrecarga con `VideoSource`.
    bool open(const uint8_t* ivf, size_t n, std::string* err);

    bool     is_open()     const;
    uint32_t frame_count() const;
    uint32_t width()       const;
    uint32_t height()      const;
    /// fps declarados en el time base del IVF (0 = ausente). El reproductor
    /// mapea frames de JUEGO a frames de CLIP con esta razón.
    double   fps()         const;

    /// Decodifica el frame `index` y devuelve su vista, o nullptr si falla.
    ///
    /// IDEMPOTENTE: pedir el mismo índice dos veces NO re-decodifica y NO mueve
    /// `seq`. Es un requisito, no una optimización — produce_frame no es 1:1 con
    /// los frames emulados (el compose re-produce, replay_invalidate re-produce,
    /// el export re-produce), y sin idempotencia el video avanzaría por cada
    /// re-render en vez de por cada frame de juego.
    const VideoFrameView* decode(uint32_t index);

    /// Valida un .ivf sin instanciar nada. Para el bake: nunca se hornea un
    /// video que no se pueda decodificar en destino.
    ///
    /// Exige que TODOS los frames sean keyframes, y eso es de fase 1 y no una
    /// preferencia: hace que decodificar el frame N sea UN solo paquete, sin
    /// caminar el GOP ni mantener máquina de seek. Es lo que la decisión «la
    /// posición sale del contenido» exige de verdad — un salto al medio tiene
    /// que caer en el frame correcto sin decodificar todo lo anterior.
    static bool validate(const uint8_t* ivf, size_t n,
                         VideoInfo* out, std::string* err);

    /// Igual, sobre una fuente. La RAM que usa es el paquete MÁS GRANDE del
    /// clip, no el clip: es lo que permite que el bake valide un video de 8K
    /// sin reservar un giga ().
    static bool validate(VideoSource& src, VideoInfo* out, std::string* err);

    /// Si el engine se construyó con libvpx. Sin esto, todo lo demás falla con
    /// un motivo legible en vez de fallar raro.
    static bool available();

    /// Costo acumulado, en ms, separado en sus dos mitades. Existe porque son
    /// dos problemas DISTINTOS con arreglos distintos, y el desglose fue lo que
    /// decidió : la conversión I420→BGRA era el 62% del costo por frame y se
    /// eliminó pasándola al fragment shader; el decode VP9 no se puede mover, y
    /// sólo baja con menos resolución o más hilos ().
    ///
    /// Desde  `convert` queda en CERO: no hay conversión en CPU. Se conserva
    /// en la firma —y no se borra— porque es la MEDIDA de que sigue en cero; si
    /// alguna vez vuelve a subir, el oráculo lo ve.
    void cost_ms(double* decode, double* convert, uint32_t* frames) const;

    /// Hilos con los que se abrió el decoder VP9 (row-mt). Cero si no está
    /// abierto. Va junto al costo porque son el mismo número leído de dos
    /// maneras: 38,9 ms por frame en un hilo no dice lo mismo que en ocho.
    unsigned threads() const;

    /// RAM que se queda el clip abierto: índice de paquetes + paquete en curso +
    /// frame convertido + lo que retenga la fuente. Por streaming NO depende del
    /// tamaño del video, y ésa es toda la tesis de  — así que es un número
    /// que hay que poder mirar, no una promesa (tools/video_stream_smoke).
    uint64_t resident_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// : mide el costo de reproducir `clip` decodificando frames CONSECUTIVOS
/// durante al menos `measure_ms`. false si no se pudo medir (clip vacío, o un
/// decode que falló) — y en ese caso el llamador NO tiene que inventar un
/// veredicto.
///
/// POR QUÉ TARDA MÁS DE UN SEGUNDO. El costo por frame no es una constante de la
/// máquina: en ráfaga corta el CPU puede estar en turbo y dar más de lo que
/// sostiene, y una medición de medio segundo mediría eso. Se mide más de un
/// segundo y se reporta el promedio de la SEGUNDA MITAD, que es post-boost;
/// `ms_burst` deja la primera visible como guardia (ver VideoProbe).
///
/// Calibrado contra el costo sostenido real en tools/video_stream_smoke, que
/// decodifica cientos de frames seguidos y compara: el probe tiene que quedar en
/// el mismo orden de magnitud, y si se separa el aviso al artista miente.
///
/// CONSECUTIVOS y no repartidos: la conversión escribe un frame entero por
/// frame, y en playback eso pasa 60 veces por segundo seguidas. Saltear le da al
/// prefetcher condiciones que la reproducción no tiene. Arranca por el medio del
/// clip y da la vuelta: un video suele empezar en negro, y un frame negro se
/// decodifica mucho más rápido que uno cargado.
///
/// Recibe un clip YA ABIERTO para no decidir de dónde salen los bytes: se puede
/// medir tanto lo que se autora en disco como lo que va a correr desde el pack.
bool video_probe(VideoClip& clip, double measure_ms, VideoProbe* out);

}  // namespace ayther
