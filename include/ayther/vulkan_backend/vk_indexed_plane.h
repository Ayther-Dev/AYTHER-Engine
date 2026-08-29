#pragma once
// ---------------------------------------------------------------------------
// VkIndexedPlane — el pipeline INDEXADO del render propio (R-2, ).
//
// En vez de decodificar tiles a texturas RGBA, sube el ESTADO CRUDO del VDP y
// resuelve el color en el fragment shader:
//   - VRAM (2048 patrones 4bpp) → textura R8_UINT de 512×256 (64 tiles/fila,
//     cada tile 8×8 texels = su nibble de índice de color desempaquetado).
//   - CRAM (64 colores empaquetados R0-2/G3-5/B6-8) → textura 64×1 RGBA8,
//     convertida con la MISMA expansión de color que el renderer del core
//     (3 bits → nivel ×2 → RGB565 → 888 por replicación de bits), para que el
//     resultado sea comparable BIT A BIT contra el framebuffer del emulador.
//   - Cada quad de 8×8 px lleva patrón + línea de paleta + flips por push
//     constant; el shader hace índice → color. El índice 0 se descarta
//     (semántica del VDP: transparente).
//
// Por qué así (de la épica ): un fundido de paleta ya no invalida nada
// (cambian 64 texels), y un efecto por elemento es un uniform por quad, no una
// lane nueva. Las subidas son INCREMENTALES: shadow CPU de VRAM/CRAM y sólo
// los tiles/paleta que cambiaron viajan a la GPU.
//
// Contrato de layouts (idéntico a VkSprite): el render target debe estar en
// COLOR_ATTACHMENT_OPTIMAL al entrar a draw_cells(); el pass lo deja en
// TRANSFER_DST_OPTIMAL. upload_*() se graban FUERA de un render pass.
// Contrato de orden: al menos un upload_vram()+upload_cram() antes del primer
// draw_cells() (las imágenes nacen UNDEFINED; draw sin upload es no-op).
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

struct VmaAllocation_T;
using  VmaAllocation = VmaAllocation_T*;

class VkContext;
class VkRenderTarget;

class VkIndexedPlane {
public:
    VkIndexedPlane()  = default;
    ~VkIndexedPlane();

    VkIndexedPlane(const VkIndexedPlane&)            = delete;
    VkIndexedPlane& operator=(const VkIndexedPlane&) = delete;

    bool init(VkContext& ctx, const VkRenderTarget& target,
              const char* vert_spv_path, const char* frag_spv_path);
    /// R-5: el offscreen cambió (resize) — recrea SOLO el framebuffer sobre el
    /// view nuevo (pipeline, texturas y shadows persisten; el formato no cambia).
    bool rebuild(VkContext& ctx, const VkRenderTarget& target);
    void shutdown(VkContext& ctx);
    bool is_ready() const { return pipeline_ != VK_NULL_HANDLE; }

    /// VRAM cruda del core (64 KB, vista word-swapped del host — misma que
    /// video_ram()). Compara contra el shadow por tile (32 bytes) y sube SÓLO
    /// los tiles que cambiaron (una región de copia por tile sucio).
    void upload_vram(VkContext& ctx, VkCommandBuffer cmd,
                     const uint8_t* vram, size_t size);

    /// CRAM cruda del core (128 bytes, layout GPX 0000BBB0GGG0RRR0). Si cambió,
    /// reconvierte los 64 colores y sube la paleta entera (256 bytes).
    void upload_cram(VkContext& ctx, VkCommandBuffer cmd,
                     const uint8_t* cram, size_t size);

    /// Un tile de plano en pantalla: destino en px del canvas + identidad.
    /// R-5: w/h del quad en px del canvas — el compose escala los 8×8 nativos
    /// al canvas (p.ej. ×6 en 1920); el default 8 mantiene el uso 1:1.
    struct CellQuad {
        float    x = 0, y = 0;   ///< esquina sup-izq en px del canvas
        float    w = 8, h = 8;   ///< tamaño del quad en px del canvas
        uint16_t pattern = 0;    ///< índice de patrón 0..2047
        uint8_t  pal     = 0;    ///< línea de paleta 0..3
        uint8_t  flips   = 0;    ///< bit0 hflip · bit1 vflip
        /// R-6 (): efectos por quad. fx = tinte Q2.6 r|g<<8|b<<16 (64 =
        /// neutro) + opacidad<<24 (255 = opaco); el default es EXACTAMENTE el
        /// camino sin efectos (byte-idéntico — validado por scene_compose).
        /// flat_rgba ≠ 0 = modo silueta: color plano AABBGGRR donde idx≠0.
        uint32_t fx       = 0xFF404040u;
        uint32_t flat_rgba = 0;
        /// R-8 (): modo UV checker — bit0 = on · bit1 = par de colores
        /// (0 ámbar «nunca autorado» · 1 magenta «el asset no cargó») ·
        /// bits 8-19/20-31 = offset local del quad dentro del ELEMENTO en px
        /// de emu (ancla el patrón al elemento, no a la pantalla). 0 = normal.
        uint32_t checker  = 0;
        ///  (runtime_enhancement): 1 = el quad se dibuja con el escalador
        /// EPX/Scale2x sobre ÍNDICES de paleta (bit 4 de `attr` en el push
        /// constant). 0 = camino normal, byte-idéntico al de siempre.
        uint8_t  enhance  = 0;
        /// : intensidad del suavizado 0..255 (bits 5-12 de `attr`);
        /// 255 = vector limpio. Sólo cuenta con enhance = 1.
        uint8_t  enhance_k = 255;
        ///  v2/v3: los 8 TILES VECINOS del quad para el escalador — en
        /// espacio de PANTALLA, 14 bits cada uno: bits 0-10 patrón · 11 hflip
        /// · 12 vflip · 13 válido. nb0 = izq | der<<14 · nb1 = arriba |
        /// abajo<<14 · nb2 = arriba-izq | arriba-der<<14 · nb3 = abajo-izq |
        /// abajo-der<<14. Con el anillo 3×3 el shader alcanza cualquier píxel
        /// a distancia 2 (lo que pide xBR). 0 = sin vecino (se clampea al
        /// tile). Sólo se llenan con `enhance`.
        uint32_t nb0 = 0, nb1 = 0, nb2 = 0, nb3 = 0;
        static uint32_t neighbor(uint16_t pattern, uint8_t flips) {
            return (uint32_t)(pattern & 0x7FF) | ((uint32_t)(flips & 3) << 11) | (1u << 13);
        }
    };

    /// Dibuja los quads (un draw + push constant por celda — el modelo de la
    /// épica: un efecto por elemento = un uniform por quad). scissor_x recorta
    /// columnas de la izquierda (left-column blanking del VDP, reg 0 bit 5).
    void draw_cells(VkContext& ctx, VkCommandBuffer cmd,
                    const CellQuad* cells, size_t count,
                    uint32_t canvas_w, uint32_t canvas_h, int32_t scissor_x = 0);

    /// La conversión de color ÚNICA (CRAM empaquetada → RGBA8) que usan la
    /// paleta subida y cualquier comparador CPU (el smoke). Igual que el core:
    /// canal de 3 bits → nivel normal ×2 (0..14) → RGB565 → 888 replicando
    /// bits. Devuelve 0xAABBGGRR (R en el byte bajo — orden de memoria RGBA).
    static uint32_t genesis_color_rgba(uint16_t packed);

private:
    VkContext* context_ = nullptr;
    bool create_images(VkContext& ctx);
    bool create_pipeline(VkContext& ctx, VkFormat fmt,
                         const char* vert_spv_path, const char* frag_spv_path);

    // Textura de índices (512×256 R8_UINT) + staging persistente.
    VkImage       idx_image_ = VK_NULL_HANDLE;
    VkImageView   idx_view_  = VK_NULL_HANDLE;
    VmaAllocation idx_alloc_ = nullptr;
    VkBuffer      idx_staging_       = VK_NULL_HANDLE;
    VmaAllocation idx_staging_alloc_ = nullptr;
    void*         idx_staging_map_   = nullptr;
    bool          idx_uploaded_ = false;   ///< primera subida hecha (layout definido)

    // Paleta (64×1 RGBA8) + staging persistente.
    VkImage       pal_image_ = VK_NULL_HANDLE;
    VkImageView   pal_view_  = VK_NULL_HANDLE;
    VmaAllocation pal_alloc_ = nullptr;
    VkBuffer      pal_staging_       = VK_NULL_HANDLE;
    VmaAllocation pal_staging_alloc_ = nullptr;
    void*         pal_staging_map_   = nullptr;
    bool          pal_uploaded_ = false;

    // Shadows CPU para la subida incremental.
    uint8_t vram_shadow_[0x10000] = {};
    uint8_t cram_shadow_[0x80]    = {};
    bool    vram_seen_ = false;    ///< false = primer upload → todo sucio
    bool    cram_seen_ = false;

    // Pipeline.
    VkRenderPass          render_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_    = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_    = VK_NULL_HANDLE;
    VkSampler             sampler_     = VK_NULL_HANDLE;   // NEAREST (texelFetch igual lo exige)
    VkFramebuffer         framebuffer_ = VK_NULL_HANDLE;
    VkExtent2D            extent_      = {};
};
