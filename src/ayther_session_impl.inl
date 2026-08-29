// ---------------------------------------------------------------------------
// Impl — owns every motor handle + the per-frame buffers FrameView points into.
// ---------------------------------------------------------------------------
struct AytherSession::Impl {
    RetroRunner runner;                       // emulator host
    std::string core_path, rom_path;

    TileHasherPtr   tile_hasher;              // ayther_core handles (RAII)
    TileSubPtr      tile_sub;
    TileSubPtr      plane_sub;                // Fase 2c: catálogo hash→asset de tiles de plano
    SpriteHasherPtr sprite_hasher;
    SpriteSubPtr    sprite_sub;
    PoseSubPtr      pose_sub;       // CU-AN multi-sprite: sustitución por firma de pose
    TweenPtr        tween;          // CU-AN in-betweens: playback por tiempo
    AudioHasherPtr  audio_hasher;
    AudioEventPtr   audio_event_det;   // C-A2: eventos por comandos de chip (recording-céntrico)
    AudioSubPtr     audio_sub;
    ScriptPtr       script;
    PackPtr         pack;
    std::string     pack_path;

    bool        audio_enabled = false;        // HD audio output (motor-owned)
    bool        vram_warned   = false;        // one-shot: core exposes no VRAM
    bool        poke_dirty    = false;        // M5: navegada por poke → REC off
    ///  EM-7.3: cheats del JUGADOR, reaplicados cada frame. Un vector y no
    /// un mapa: son unos pocos, el orden es el que el jugador los agregó, y dos
    /// cheats sobre la misma dirección son un conflicto suyo que no nos toca
    /// resolver.
    struct CheatEntry { uint32_t address; uint16_t value; };
    std::vector<CheatEntry> cheats;
    AudioPlayer audio;

    // Modo 3 (RAM anchoring): perfil de juego + sustitución HD por instancia.
    // resolve() corre en produce_frame con la cámara del plano A leída del VDP
    // (mismo camino que el resolver de Fase 2c, validado por tools/mode3_spike).
    Mode3Resolver mode3;

    // Fondos (Componentes): captura del stitcher para el export por capa. Con
    // bg_capture_on, produce_frame acumula las celdas visibles de A/B en espacio
    // de nivel; los unwrappers (uno por eje/plano) se crean lazy al primer frame
    // con geometría de plano (necesitan el período wpx/hpx de los VDP regs).
    bool         bg_capture_on = false;
    /// : corte de escena detectado (delta de scroll NO físico) → el stitch
    /// se CONGELA (conserva el nivel acumulado; la escena nueva no contamina).
    /// Se re-arma al (re)iniciar la captura.
    bool         bg_scene_cut  = false;
    BgStitcherPtr bg_st;
    /// TODAS las parejas (posición, hash) observadas por plano, acumuladas en
    /// paralelo al stitcher. El stitcher guarda códigos de nametable (para
    /// re-dibujar); esto guarda la identidad con la que la Panorámica se
    /// RECONOCE en runtime.
    ///
    /// Son TODAS y no la primera de cada posición: una celda animada muestra
    /// hashes distintos en la MISMA posición de nivel, y quedarse con el primero
    /// deja a los demás estados sin posición conocida — al anclar votan por
    /// cualquier otro lugar donde ese hash aparezca. Medido en Aladdin: con la
    /// primera sola, la moda del voto llegaba al 27% y no anclaba ni un frame de
    /// los que fallaban; con todas, al 100%.
    std::set<std::pair<uint64_t, uint64_t>> bg_hash[3];   // (poskey, hash)
    /// : el hash que la LÁMINA DIBUJA en cada posición — el que
    /// corresponde al código que el PNG conserva (`Cell::last` del stitcher,
    /// `background.rs`), no cualquiera de los que pasaron por ahí.
    ///
    /// Existe porque `bg_hash` guarda TODAS las lecturas (cada estado de una
    /// celda animada tiene que poder anclar) y el PNG guarda UNA. Verificar la
    /// cobertura contra el conjunto entero declara «100 %» sobre una lámina que
    /// muestra otro tramo del nivel: medido en Sonic 3 & K f2092, anclada con
    /// cobertura 100 % y el recorte mostrando Angel Island en una cueva.
    ///
    /// Se actualiza en el MISMO punto y con la MISMA política que el stitcher
    /// —último gana— así que los dos no se pueden separar sin que alguien
    /// cambie las dos líneas a la vez.
    std::unordered_map<uint64_t, uint64_t> bg_hash_drawn[3];
    int32_t bg_camx[3] = {}, bg_camy[3] = {};
    bool    bg_cam_ok[3] = {};
    // : bg_uyB — el plano B scrollea en vertical por SU entrada de VSRAM
    // (la impar); usar la de A dejaba al stitcher ciego a la subida de GA (la
    // tira quedaba clavada en 28 filas y el contenido nuevo pisaba al viejo).
    UnwrapPtr    bg_uxA, bg_uyA, bg_uxB, bg_uyB;
    // : cámara de CONTENIDO — juegos que scrollean reescribiendo la
    // nametable (GA sube el plano B corriendo el contenido en VRAM con el
    // registro fino volviendo a 0) mueven el nivel sin mover los registros.
    // Se detecta comparando la grilla visible contra la del frame anterior y
    // el delta extra (en CELDAS) se acumula acá; las coordenadas de NIVEL
    // (stitcher + bg_hash) suman este offset — la lectura de nametable sigue
    // por registros, que es lo físico.
    int32_t bg_content_col[3] = {}, bg_content_row[3] = {};
    std::vector<uint16_t> bg_prev_grid[2];   // codes visibles del frame previo (A/B)
    int32_t bg_prev_col[2] = {}, bg_prev_row[2] = {};
    bool    bg_prev_ok[2] = {};
    //  EM-8.0: la cámara ABSOLUTA de cada banda de parallax, por plano. El
    // plano B lleva una entrada de Hscroll por banda, así que «columna de nivel»
    // depende de la FILA; con una cámara única las bandas se apilan unas sobre
    // otras. Tiene estado porque el des-enrollado lo pide: cuántas vueltas dio
    // una banda no se deduce de un solo frame.
    ayther::BandCameras bg_bands[2];
    // : frames seguidos con la cámara del plano QUIETA — con quietud se
    // leen también las últimas columnas (el skip anti-streaming pierde el borde
    // derecho del extremo final del paneo: nadie vuelve a verlo).
    int     bg_static_frames[2] = {};

    // Audios C-A2 (Componentes): sustitución HD por EVENTO. Las ventanas se
    // resuelven sobre los eventos de la toma (resolve_audio_events); el bloque
    // de audio de produce_frame consulta mute_at/triggers_at por frame. El
    // cache de eventos permite re-resolver al (des)asignar sin re-detectar.
    AudioEventSubstitution        audio_evt;
    std::vector<AytherAudioEvent> audio_events_cache;

    // Animaciones C-S2 (Componentes): playback HD en fase. resolve() corre en
    // produce_frame sobre las sprite occurrences; FrameView publica los
    // AnimHdFrame y el renderer los dibuja con VkSprite::draw_anim.
    AnimationPlayer anim;

    // Lab authoring overrides (hash → asset + ref cromática E1 ).
    // Re-applied to the sprite substitutor every frame AFTER the Lua overrides,
    // so they persist across the per-frame clear and a script can still
    // override them per-context. ref_rgb {0,0,0} = sin ref → peak-hold gris.
    struct LabSpriteAssign { std::string asset; uint8_t ref_rgb[3] = {0, 0, 0}; };
    std::unordered_map<uint64_t, LabSpriteAssign> lab_sprite_overrides;
    // Sustitución de pose TRANSITORIA (todo elemento es una Pose): se aplican al
    // pose_sub cada frame ANTES del resolve y NUNCA se serializan. Con rel →
    // matching instanciado exacto. hd=true → su región limpia los sueltos.
    std::vector<AytherSession::PosePreview> preview_pose_overrides;
    // Ocultado COMPUESTO por hash (Posar): A completo → B con estos hashes
    // suprimidos (slots de las occs de A) → base = B sólo en sus rects.
    std::unordered_set<uint64_t> hidden_sprite_hashes;
    /// R-4 (): la visibilidad por ELEMENTO es una propiedad del inventario
    /// — set_hidden_elements guarda acá y los sets que YA leen los
    /// consumidores (hidden_sprite_hashes / plane_tiles_hidden) pasan a ser la
    /// UNIÓN canal-del-Lab ∪ elementos, recomputada en los setters
    /// (rebuild_hidden_sets). Un hash de sprite es inerte en el dominio de
    /// planos y viceversa, así que la unión no necesita ruteo. Costo asumido:
    /// un hash de plano en el set de sprites enciende hide_compose (un render
    /// B de más por produce) sin cambio visual — sólo mientras hay ocultos.
    std::vector<uint64_t> lab_sprite_hidden;   // canal existente (Posar/poses)
    std::vector<uint64_t> lab_plane_hidden;    // canal existente (Editar/Pintar)
    /// R-6 (): efectos por elemento, indexados por capa (misma identidad
    /// (capa,hash) que el ocultado — y por la misma razón: un gráfico puede
    /// existir en los dos dominios). El inventario los resuelve a fx_*.
    std::unordered_map<uint64_t, ElementEffect> element_fx[4];
    std::vector<AytherSession::HiddenElement> element_hidden;  // R-4: (capa,hash)
    /// : identidades a MEJORAR por software, por capa — UNIÓN de dos
    /// fuentes separables: el Lab (set_enhanced_elements, lista viva) y el pack
    /// ([[enhance]] de elements.toml, se vuelca en load_pack_into y se limpia
    /// al cambiar de pack). El inventario lee sólo `element_enhance`.
    // hash -> k (). Unión Lab ∪ pack: el Lab GANA si el mismo (capa,
    // hash) viene de los dos (insert no pisa) — lo que se está autorando
    // manda sobre lo horneado.
    std::unordered_map<uint64_t, uint8_t> element_enhance_lab[4];
    std::unordered_map<uint64_t, uint8_t> element_enhance_pack[4];
    std::unordered_map<uint64_t, uint8_t> element_enhance[4];
    bool element_enhance_any = false;
    void rebuild_enhance_sets() {
        element_enhance_any = false;
        for (int l = 0; l < 4; ++l) {
            element_enhance[l] = element_enhance_lab[l];
            element_enhance[l].insert(element_enhance_pack[l].begin(),
                                      element_enhance_pack[l].end());
            if (!element_enhance[l].empty()) element_enhance_any = true;
        }
    }
    void rebuild_hidden_sets() {
        hidden_sprite_hashes.clear();
        for (uint64_t h : lab_sprite_hidden) hidden_sprite_hashes.insert(h);
        plane_tiles_hidden.clear();
        for (uint64_t h : lab_plane_hidden) plane_tiles_hidden.insert(h);
        // Ruteo por CAPA: el mismo hash puede existir en ambos dominios (un
        // gráfico usado como sprite Y como tile de plano) — sin el ruteo,
        // ocultar el sprite ocultaba también las celdas (element_hidden_smoke).
        for (const AytherSession::HiddenElement& e : element_hidden)
            (e.layer == 3 ? hidden_sprite_hashes : plane_tiles_hidden).insert(e.hash);
        // La máscara por (plano,patrón,paleta) se re-arma en produce_frame con
        // las occurrences del frame; si ya no hay ocultos, apagar de inmediato.
        if (plane_tiles_hidden.empty()) plane_tile_suppress_any = false;
    }
    std::unordered_map<uint64_t, std::string> lab_tile_overrides;
    std::unordered_map<uint64_t, std::string> lab_audio_overrides;
    std::unordered_map<uint64_t, std::string> lab_plane_overrides;   // Fase 2c (tiles de plano)

    // Rewind (R6) — compressed savestate ring, captured at the end of step().
    RewindBuffer rewind;
    std::vector<uint8_t> rewind_scratch;   // reused serialize buffer (no per-frame alloc)
    float speed = 1.0f;                    // fast-forward multiplier (frontend reads it)

    // Recording (R7) — capture the input stream + an initial state for .arp.
    bool                  rec_active = false;
    std::vector<uint16_t> rec_inputs;
    std::vector<FrameStat> rec_stats;      // per-frame occurrence summary (R7b)
    std::vector<uint64_t> rec_hashes;      // flat sprite-hash history (R7c, CSR)
    std::vector<uint32_t> rec_hash_off;    // CSR offsets, starts {0}
    std::vector<uint64_t> rec_audio_hashes; // flat audio-hash history (.arp v7, CSR)
    std::vector<uint32_t> rec_audio_off;    // CSR offsets, starts {0}
    std::vector<uint8_t>  rec_initial;     // savestate at record start
    uint16_t              last_input0 = 0; // most recent port-0 input (logged each step)

    // Aislar capas (Lab Editar): máscara de capas DESEADA (bits A/B/Window/
    // Sprites). Se aplica SOLO en produce_frame (el frame visible); tras él, los
    // bits de SPRITES se restauran para que la re-simulación "bare" corra con
    // sprites completos → el status del VDP (overflow/colisión que el juego lee)
    // no cambia y el replay no diverge. Los planos no afectan status.
    uint8_t               layer_mask_want = 0xFF;
    bool                  layer_dim_want = false;    // atenuar capas no-sprite al 25% (0x108)
    uint8_t               suppress_want[16] = {0};   // slots SAT a ocultar (0x103)
    bool                  suppress_any = false;      // ¿algún slot suprimido?
    uint8_t               tile_suppress_want[512] = {0};  // celdas de tile a ocultar (0x104)
    bool                  tile_suppress_any = false;      // ¿alguna celda suprimida?
    // Tiles de PLANO a ocultar (0x105, Fase 2b). El Lab da hashes; la máscara por
    // (plano,patrón,paleta) se re-arma en produce_frame con las occurrences del
    // frame (idéntico esquema produce-only que tile_suppress). 3×1024 = 3072 bytes.
    std::unordered_set<uint64_t> plane_tiles_hidden;       // hashes ocultos (del Lab)
    // Plane SETS (Pintar Fase C): sustitución HD por ELEMENTO multi-tile.
    struct PlaneSetDef {
        uint8_t  plane = 0;
        uint16_t w_cells = 0, h_cells = 0;
        std::vector<AytherSession::PlaneSetMember> members;
        std::string asset;
        /// Referencia del tinte E1 (promedio RGB 0-255 de la línea CRAM del
        /// elemento al capturarlo). {0,0,0} = sin referencia → quad sin tinte.
        uint8_t ref_rgb[3] = { 0, 0, 0 };
        /// : re-anclaje del HUD al ensanchar. Ver PackPlaneSet::off_x.
        int16_t off_x = 0, off_y = 0;
    };
    std::unordered_map<uint64_t, PlaneSetDef> plane_sets;
    // ANIMACIÓN (): secuencia de plane sets con RELOJ PROPIO. Ver el
    // header para por qué es reproductor y no seguidor del contenido.
    struct PlaneSeqDef {
        std::vector<uint64_t>    steps;    ///< ids de plane set, EN ORDEN
        std::vector<std::string> assets;   ///< asset por paso ("" = el del set)
        std::vector<uint16_t>    durs;     ///< frames por paso (0 = kSeqDefaultDur)
        uint32_t                 total = 0;  ///< suma de duraciones, cacheada
    };
    /// Cadencia por defecto de un paso sin `~dur`: ≈7,5 fps, el mismo hold del
    /// player de preview del Lab (kKfHoldTicks) — así lo que se autoró mirando
    /// el preview se ve igual en el runtime.
    static constexpr uint16_t kSeqDefaultDur = 8;
    std::unordered_map<uint64_t, PlaneSeqDef> plane_seqs;
    /// Índice inverso set → [(seq, paso)]. El matcher entrega ids de SET; esto
    /// dice si ese set pertenece a una Animación. Ordenado para que, ante
    /// empate, siempre gane la misma (determinismo, no orden de hash).
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, uint32_t>>> set_to_seq;
    /// Reloj por Animación. `anchor` es el frame de juego que corresponde al
    /// t=0 del ciclo; la posición se recalcula por aritmética ABSOLUTA, nunca
    /// con un acumulador (mismo criterio que el video de la Cinemática: un
    /// acumulador se desincroniza con el primer re-produce y no se recupera).
    struct PlaneSeqClock { int64_t anchor = -1; int64_t last_seen = -1; };
    std::unordered_map<uint64_t, PlaneSeqClock> seq_clocks;

    void plane_seq_reindex() {
        set_to_seq.clear();
        for (const auto& [id, d] : plane_seqs)
            for (uint32_t i = 0; i < d.steps.size(); ++i)
                set_to_seq[d.steps[i]].emplace_back(id, i);
        for (auto& [sid, v] : set_to_seq) std::sort(v.begin(), v.end());
    }

    /// Paso vigente de `id` en este frame, o UINT32_MAX si la Animación no
    /// existe o está vacía. Re-ancla cuando estuvo AUSENTE varios frames: si se
    /// re-anclara en cada aparición, un elemento que entra y sale de pantalla
    /// reiniciaría el ciclo constantemente y nunca pasaría del primer paso.
    uint32_t plane_seq_step(uint64_t id) {
        auto it = plane_seqs.find(id);
        if (it == plane_seqs.end() || it->second.steps.empty()) return UINT32_MAX;
        const PlaneSeqDef& d = it->second;
        PlaneSeqClock& c = seq_clocks[id];
        constexpr int64_t kGapFrames = 8;   // tolerancia de ausencia
        if (c.anchor < 0 || (int64_t)frame_index - c.last_seen > kGapFrames ||
            (int64_t)frame_index < c.last_seen)
            c.anchor = (int64_t)frame_index;
        c.last_seen = (int64_t)frame_index;
        const uint32_t total = d.total ? d.total : 1u;
        int64_t t = ((int64_t)frame_index - c.anchor) % (int64_t)total;
        if (t < 0) t = 0;
        return ayther::plane_sequence_step_at(d.durs.data(),
                                              (uint32_t)d.durs.size(),
                                              (uint64_t)t, kSeqDefaultDur);
    }
    // CUADRO (CU001): pantallas estáticas declaradas + estado del match.
    struct ScreenDef {
        uint8_t     mask = 0;
        float       min_match = 0.92f, max_extra = 0.08f;
        std::string asset;
        /// Firma y conteo POR CAPA. Separadas y no sumadas: sumarlas puede
        /// cancelarse (A sube en X y B baja en X y el total no se mueve) y,
        /// sobre todo, pierde CUÁL capa cambió — que es lo que hace falta para
        /// una pantalla con parallax, donde una capa está fija y la otra se
        /// desplaza. Un Cuadro sólo mira las capas que DECLARA: lo que pase en
        /// las otras no lo invalida.
        uint64_t    sig_plane[3]   = {0, 0, 0};
        uint32_t    cells_plane[3] = {0, 0, 0};
        std::unordered_map<uint32_t, uint64_t> cells;   // key(plano,col,fila) → hash
        ///  mecanismo 2: hashes DISTINTOS por capa, sin posición — el
        /// universo del gate por presencia. Derivado de `cells` al declarar.
        std::unordered_set<uint64_t> hashes_plane[3];
    };
    std::unordered_map<uint64_t, ScreenDef> screens;
    uint64_t        screen_active = 0;    // Cuadro vigente (0 = ninguno)
    uint64_t        screen_cand   = 0;    // candidato esperando confirmación
    int             screen_streak = 0;    // frames consecutivos del candidato
    /// El frame vino de un SALTO (scrub), no del avance continuo. La histéresis
    /// de 2 frames existe para el playback —un wipe puede acertar una firma por
    /// un frame suelto— pero al saltar sólo se produce el frame de destino: el
    /// streak se quedaba en 1 y el Cuadro no se activaba NUNCA mientras se
    /// navegaba a mano, que es justo como se autora.
    bool            screen_jump   = false;
    float           screen_score = 0.0f, screen_extra = 0.0f;
    ///  mecanismo 2: ids cuyo contenido está presente este frame.
    uint64_t        screen_presence[8] = {};
    uint32_t        screen_presence_n  = 0;
    AytherSpriteSub screen_sub{};         // el quad a pantalla completa
    uint32_t        screen_sub_n = 0;

    // CINEMÁTICA (CU004): una SECUENCIA ORDENADA de Cuadros. Lo que agrega
    // sobre un Cuadro suelto no es el dibujo —eso ya lo hace el Cuadro— sino el
    // ORDEN: desambigua dos pantallas idénticas que aparecen en cinemáticas
    // distintas, y da la semántica de cancelación del spec (si el jugador
    // aprieta Start y el juego salta a un menú, la secuencia se corta y se
    // re-evalúa la pantalla nueva).
    struct KinematicDef {
        std::vector<uint64_t>    steps;    ///< ids de Cuadro, EN ORDEN
        std::vector<std::string> assets;   ///< asset por paso ("" = el del Cuadro)
        /// Frame del clip en que arranca cada paso, cuando el asset es video
        /// (). Es lo que deja que UN video cubra varios pasos.
        std::vector<uint32_t>    video_offsets;
        uint32_t gap = 12;                 ///< frames tolerados sin Cuadro confirmado
        /// El video CICLA si es más corto que el tramo (ver KinematicMedia).
        bool        loop = false;
        /// Pista de audio del video (asset aparte: el IVF es sólo video).
        std::string audio;
        float       gain = 1.0f;        ///< volumen de la pista de la Cinemática
        float       game_gain = 1.0f;   ///< ducking de la banda sonora del juego
    };
    std::unordered_map<uint64_t, KinematicDef> kinematics;
    /// Índice inverso screen_id → [(kin_id, paso)]. El matcher de Cuadro entrega
    /// UN id por frame; esto dice qué cinemáticas lo esperan y en qué posición.
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t,uint32_t>>> screen_to_kin;
    uint64_t kine_active = 0;      ///< cinemática en curso (0 = ninguna)
    uint32_t kine_step   = 0;      ///< paso vigente dentro de la secuencia
    uint32_t kine_gap    = 0;      ///< frames seguidos sin Cuadro confirmado
    /// Último frame en que el cursor AVANZÓ. produce_frame NO es 1:1 con los
    /// frames emulados —corre de nuevo en el re-render bare del compose, en
    /// export_frame y en replay_invalidate— y el matcher de Cuadro sobrevive a
    /// eso porque es una función PURA del frame. Una máquina de estados no: sin
    /// este gate, re-producir el mismo frame haría avanzar la secuencia.
    int64_t  kine_last_frame = -1;

    // VIDEO del paso (). Los clips se abren perezosamente la primera vez que
    // un paso los pide y se quedan cacheados por ruta. Desde  lo cacheado
    // es el ÍNDICE y la fuente, no el archivo: un clip abierto ocupa un paquete
    // más un frame, mida el video 30 MB o 1 GB.
    std::unordered_map<std::string, std::unique_ptr<ayther::VideoClip>> videos;
    /// Fase del video, DELIBERADAMENTE fuera de lo que `kinematic_reset()`
    /// borra, y anclada por identidad `(cinemática, paso)` en vez de por un
    /// contador propio.
    ///
    /// El motivo es una cadena real: el Lab adelanta el playhead por reloj hasta
    /// 16 frames y recién ahí emite UN `replay_seek`; el motor lo resuelve con
    /// fast-forward bare y produce sólo el frame terminal; entonces
    /// `fnow != kine_last_frame + 1` y corre `kinematic_reset()`. Hoy eso es
    /// benigno porque la re-entrada por contenido cae en el mismo paso. Con un
    /// contador «frames desde que arrancó el paso» adentro del estado reseteable,
    /// CADA hipo de playback rebobinaría el video al inicio del plano.
    ///
    /// Con el ancla por identidad: si el paso sigue siendo el mismo, el ancla
    /// sobrevive y la posición se recalcula por aritmética ABSOLUTA
    /// (`frame_index - anchor`), sin acumulador que se desincronice.
    struct VideoPhase {
        uint64_t kin    = 0;
        uint32_t step   = 0;
        int64_t  anchor = -1;
    } vid;
    ayther::VideoFrameView vid_out{};
    bool                   vid_on = false;

    /// AUDIO de la Cinemática. El IVF es sólo video, así que la pista viaja
    /// como asset aparte y se reproduce con el mismo criterio de ancla que la
    /// imagen — pero NO se puede re-anclar por paso: la Cinemática avanza 44
    /// pasos en cuatro segundos y reiniciar el stream en cada uno sería un
    /// tartamudeo continuo. Entonces se arranca UNA vez y sólo se re-sincroniza
    /// cuando el ancla SALTA (un scrub), que es exactamente lo que hay que
    /// corregir. `still` cuenta ticks sin avanzar: pausado se corta, porque
    /// scrubbear cuadro a cuadro con la voz corriendo no es una previsualización
    /// de nada.
    struct VideoAudio {
        uint64_t kin    = 0;
        int64_t  anchor = 0;      ///< frame de juego que mapea al t=0 del asset
        bool     on     = false;
        int64_t  last_f = -1;
        int      still  = 0;
        float    gain   = 1.0f;   ///< la última ganancia aplicada al stream vivo
    } vaud;
    /// Clave del stream (dedup/corte). Constante: hay UNA Cinemática activa.
    static constexpr uint64_t kVideoAudioKey = 0xA17E'2600'0000'0002ull;

    void video_audio_stop() {
        if (vaud.on) audio.stop_sfx_by_key(kVideoAudioKey);
        // La banda sonora vuelve SIEMPRE a su volumen, aunque no hubiera pista
        // de Cinemática: el ducking es un préstamo con lifetime propio y
        // dejarlo bajado sería un bug silencioso que sobreviviría a la escena.
        audio.set_game_gain(1.0f);
        vaud = {};
    }
    void video_reset() {
        vid = {}; vid_out = {}; vid_on = false;
        video_audio_stop();
    }

    /// Resuelve el frame de video que corresponde a ESTE frame de juego.
    /// Declarado acá y definido fuera de la clase (necesita leer del pack).
    void video_tick(const std::string& path);

    void kinematic_reindex() {
        screen_to_kin.clear();
        for (const auto& [kid, d] : kinematics)
            for (uint32_t i = 0; i < d.steps.size(); ++i)
                screen_to_kin[d.steps[i]].emplace_back(kid, i);
        // Orden determinista: el arranque elige entre candidatos y un
        // unordered_map no garantiza por cuál empieza.
        for (auto& [sid, v] : screen_to_kin) std::sort(v.begin(), v.end());
    }
    void kinematic_reset() { kine_active = 0; kine_step = 0; kine_gap = 0; }

    /// Un tick de la secuencia. `sid` = el Cuadro CONFIRMADO este frame (0 = ninguno).
    void kinematic_tick(uint64_t sid) {
        if (kine_active) {
            auto it = kinematics.find(kine_active);
            if (it == kinematics.end()) { kinematic_reset(); }
            else {
                const auto& st = it->second.steps;
                if (kine_step >= st.size()) { kinematic_reset(); }
                else if (sid == 0) {
                    // TOLERANCIA. `screen_active` cae a 0 durante UN frame en
                    // toda transición limpia de Cuadro (la histéresis exige 2
                    // frames para confirmar el nuevo), y varios más si hay un
                    // wipe. Cancelar por «no hay Cuadro» rompería la secuencia
                    // en CADA paso legítimo.
                    if (++kine_gap > it->second.gap) kinematic_reset();
                } else if (sid == st[kine_step]) {
                    kine_gap = 0;                       // sigue en el mismo paso
                } else if (kine_step + 1 < st.size() && sid == st[kine_step + 1]) {
                    ++kine_step; kine_gap = 0;          // avanza
                } else {
                    kinematic_reset();                  // Cuadro AJENO: se rompió
                }
            }
        }
        if (!kine_active && sid) {
            // Arranque. Se permite entrar en CUALQUIER paso, no sólo el
            // primero: la posición sale del CONTENIDO de la pantalla, así que
            // saltar al medio de la cinemática con un scrub cae donde
            // corresponde. Ante empate gana el paso más temprano y, a igualdad,
            // el id menor — determinismo, no orden de hash.
            auto f = screen_to_kin.find(sid);
            if (f != screen_to_kin.end() && !f->second.empty()) {
                uint64_t bk = 0; uint32_t bs = UINT32_MAX;
                for (const auto& [kid, step] : f->second)
                    if (step < bs || (step == bs && kid < bk)) { bs = step; bk = kid; }
                if (bk) { kine_active = bk; kine_step = bs; kine_gap = 0; }
            }
        }
    }
    // PANORÁMICA (CU003): la tira del nivel de una capa + anclaje por contenido.
    struct PanoramaDef {
        uint8_t     plane = 0;
        int32_t     origin_x = 0, origin_y = 0;
        uint16_t    w_cells = 0, h_cells = 0;
        std::string asset;
        /// Luma CRAM de REFERENCIA (promedio de las 4 líneas al DEFINIR la
        /// tira, con la escena a niveles normales): el tinte del quad en vivo
        /// es luma_viva/ref — con la paleta fundida a negro la tira se apaga
        /// con la escena (f33–86 de la demo mostraba la tira a todo color
        /// sobre un fundido, reporte 2026-07-30). 0.30 = fallback razonable
        /// cuando al definir no hay CRAM iluminada (carga de proyecto/pack).
        /// Queda como FALLBACK del tinte cromático de abajo.
        double      ref_luma = 0.30;
        /// Referencia CROMÁTICA del tinte (2026-08-16): el mismo mecanismo E1
        /// que ya usan los sprites, llevado a la tira. La versión luma sólo
        /// APAGA y ENCIENDE — un atardecer que vira a naranja se veía como un
        /// oscurecimiento gris, y un amanecer no se veía en absoluto porque el
        /// factor estaba topado en 1.
        ///
        /// La referencia se captura ANCLADA (peak-hold en produce_frame), no
        /// al definir: define_panorama corre en cada sync del catálogo con la
        /// CRAM de la pantalla que esté viva — capturar ahí le pegaba a la
        /// panorámica del gameplay la paleta del TÍTULO y el cociente por
        /// canal convertía esa referencia ajena en un viraje rojizo con
        /// ganancia 2× (reporte 2026-08-19).
        ///
        /// `ref_w[p]` = cuánto aportaba la línea `p` al capturar (su luma). Se
        /// usa como PESO en los dos lados del cociente, y por eso una línea
        /// negra —o una que la tira no usa— no diluye la señal: es lo que hacía
        /// imperceptible un atardecer que sólo tocaba la línea del cielo,
        /// promediado contra otras tres que no cambiaban.
        double      ref_w[4]  = { 0, 0, 0, 0 };
        /// Agregado ponderado por canal al capturar: Σ_p ref_w[p]·rgb[p][c].
        double      ref_ch[3] = { 0, 0, 0 };
        bool        ref_chroma = false;   ///< hay referencia cromática utilizable
        /// Luma del frame ANCLADO en que se capturó la referencia vigente
        /// (peak-hold, como pal_luma_peak): el primer frame matcheado la fija
        /// y uno más luminoso la re-fija — así el fade-in de una transición
        /// sube la referencia con la escena hasta el nivel normal. 0 = nunca
        /// se capturó anclada.
        double      ref_peak = 0.0;
        /// Índice de ANCLAJE: sólo los hashes RAROS de la tira (los frecuentes
        /// no discriminan y multiplican el costo del voto).
        std::unordered_map<uint64_t, std::vector<std::pair<int32_t,int32_t>>> anchors;
        /// Índice de COBERTURA: (lx,ly) → hash, la tira entera. `anchors` sólo
        /// tiene los raros y no sirve para esto: una vez anclada la cámara hay
        /// que decidir, celda por celda, si lo que se ve ES la tira o es otra
        /// cosa dibujada sobre el mismo plano (un HUD, un primer plano). Se
        /// compara el hash observado contra el de la tira en esa posición de
        /// nivel — si no coincide, esa celda NO se cubre.
        /// Posición → hashes vistos ahí. Varios cuando la celda está animada.
        std::unordered_map<uint64_t, std::vector<uint64_t>> by_pos;
        uint32_t    total_cells = 0, rare_cells = 0;
        ///  EM-8.1: % de posiciones de la tira con UN solo hash. Bajo =
        /// tira ambigua (animada o contaminada por un barrido que cruzó de
        /// zona), y entonces la verificación de cobertura no vale para
        /// decidir qué dibujar donde nadie puede corregirlo.
        uint32_t    clean_pct = 0;
    };
    /// Clave de `by_pos`. Empaqueta dos int32 de posición de nivel.
    static uint64_t pano_key(int32_t lx, int32_t ly) {
        return ((uint64_t)(uint32_t)lx << 32) | (uint32_t)ly;
    }

    // -- : matcheo de celdas TOLERANTE AL REPALETADO ---------------------
    //
    // El hash de celda mezcla el ÍNDICE de línea CRAM al final del FNV1a del
    // patrón. Eso hace que un juego que reasigna la celda a otra línea —lo que
    // hacen los ciclos de día/noche que REPINTAN en vez de cambiar el contenido
    // de la línea— produzca un hash distinto para el mismo dibujo, y la tira se
    // despegue sola: dejan de votar las celdas, la cobertura no llega al piso y
    // la Panorámica no se dibuja.
    //
    // No se puede normalizar el lado de la TIRA porque no guarda la paleta:
    // `PanoramaCell` es hash + posición, y ese es también el formato del pack.
    // Lo que sí se puede es preguntar por las otras lecturas del hash OBSERVADO:
    // `ayther_plane_tile_hash_repalette` deshace la última vuelta y la rehace
    // bajo otra línea con aritmética exacta (el PRIME es invertible mod 2^64),
    // así que las cuatro variantes son exactas, no aproximadas.
    //
    // El camino DIRECTO se prueba siempre primero: sin repaletado —el caso
    // normal— esto no cuesta nada, y el trabajo extra lo pagan sólo las celdas
    // que ya iban a descartarse.

    /// Las 4 lecturas del hash `h` observado bajo la línea `pal`. `out[0]` es el
    /// hash tal cual. Vive en el header público (con su oráculo) porque la
    /// regla es del FORMATO del hash, no de la Panorámica.
    static void pano_hash_variants(uint64_t h, uint8_t pal, uint64_t out[4]) {
        ayther_plane_tile_hash_variants(h, pal, out);
    }

    /// Posiciones de anclaje de `h` en la tira, bajo cualquier línea. null = no
    /// es una celda de anclaje.
    static const std::vector<std::pair<int32_t, int32_t>>*
    pano_find_anchor(const PanoramaDef& pd, uint64_t h, uint8_t pal) {
        auto it = pd.anchors.find(h);
        if (it != pd.anchors.end()) return &it->second;
        uint64_t var[4];
        pano_hash_variants(h, pal, var);
        for (int i = 1; i < 4; ++i) {
            it = pd.anchors.find(var[i]);
            if (it != pd.anchors.end()) return &it->second;
        }
        return nullptr;
    }

    /// ¿La celda que la LÁMINA DIBUJA en esa posición ES la observada? ()
    ///
    /// La regla vive en `panorama_cover.h` —es del FORMATO de la tira, no de la
    /// sesión— y por eso tiene oráculo propio sin ROM. Acá sólo se le inyecta
    /// la función de variantes del core.
    static bool pano_pos_matches(const std::vector<uint64_t>& strip,
                                 uint64_t h, uint8_t pal) {
        return ayther::panorama_pos_matches(strip, h, pal,
                                            &ayther_plane_tile_hash_variants);
    }

    /// Construye la def desde las celdas crudas. Vive en Impl y no dentro de
    /// `AytherSession::define_panorama` porque `load_pack_into` TAMBIÉN la
    /// necesita y es método de Impl — no tiene el objeto público. Sin esto, la
    /// carga del pack tendría que duplicar el cálculo de rareza, con su
    /// constante de tuning, y las dos copias se irían separando.
    static PanoramaDef build_panorama(uint8_t plane, int32_t ox, int32_t oy,
                                      uint16_t w, uint16_t h,
                                      const AytherSession::PanoramaCell* cells,
                                      uint32_t n, const std::string& asset);
    std::unordered_map<uint64_t, PanoramaDef> panoramas;
    ///  fase 0: ancho lógico del ensanchado (0 = apagado). Es lo que PIDE
    /// el caller (el Lab, o el runtime); el gate de EM-8.2 puede pisarlo por
    /// frame sin destruirlo — apagar el gate tiene que devolver esto intacto.
    uint32_t wide_w = 0;
    ///  EM-8.2: el gate del pack. NULL cuando el pack no declara
    /// `[[widescreen]]`, que es el caso de todos los ya horneados: entonces
    /// `wide_w` manda solo y el ensanchado manual del Lab sigue funcionando.
    ///
    /// El gate es OBLIGATORIO y no un refinamiento: el área extendida sale de
    /// la lámina, y la lámina sólo existe donde el juego recorrió. Medido con
    /// `widescreen_spike`: en una toma quieta la racha dibujable es 0 por los
    /// cuatro lados. En un menú o una pantalla de título, ensanchar no muestra
    /// el nivel — muestra el vacío.
    struct WsGateDel {
        void operator()(WidescreenGate* g) const { ayther_widescreen_gate_free(g); }
    };
    std::unique_ptr<WidescreenGate, WsGateDel> wide_gate;
    /// Ancho EFECTIVO del frame: el del gate si opinó, si no el pedido.
    uint32_t wide_w_eff = 0;
    int32_t  pano_cam_x = 0, pano_cam_y = 0;
    /// : la cámara POR BANDA de la Panorámica ganadora. Con line-scroll un
    /// plano tiene bandas que se desplazan a distinto ritmo y una sola cámara
    /// no las explica (medido: Sonic 3 & K, 37 bandas en el plano B; Golden Axe
    /// ninguna en 40.854 frames). Vacío o de un solo elemento = el modelo de
    /// siempre, y la emisión sale idéntica.
    std::vector<BandCam> pano_bandcams;
    uint64_t pano_id = 0;
    uint8_t  pano_tint[3] = { 64, 64, 64 };   // Q2.6 (64 = 1.0): fundido del quad
    uint32_t pano_votes = 0, pano_cells = 0;
    ///  EM-8.1: qué fracción de las celdas visibles del plano EXPLICA la
    /// tira en la posición anclada (0-100). Es el mismo número que decide
    /// `explains`, guardado porque el área extendida le exige más que la nativa.
    uint32_t pano_cover = 0;
    bool     pano_valid = false;
    std::vector<AytherSpriteSub> pano_subs;   // un quad por TRAMO de la tira
    /// El aviso de catálogo lleno se emite UNA vez por sesión (lección de los
    /// stat storms: un fprintf por frame es peor que el problema).
    bool     plane_occ_warned = false;
    /// Último anclaje válido, para la continuidad temporal del voto (un hash
    /// raro repetido en otro tramo del nivel produciría outliers sueltos).
    /// NO participa cuando no hay referencia — tras un seek se resuelve por
    /// moda pura, que es lo que hace que el salto siga anclando bien.
    uint64_t pano_last_id = 0;
    int32_t  pano_last_x = 0, pano_last_y = 0;
    /// Modo HD del frontend. Gatea el matcher de sets: con el HD apagado la
    /// supresión de los originales dejaría agujeros. Antes vivía sólo en el
    /// Lab (que limpiaba los sets a mano); el runtime no tenía ninguno, así que
    /// apenas los sets viajaron en el pack pasar a Original mostraba los
    /// agujeros. Default ON: una sesión sin frontend que lo maneje ve el HD.
    bool hd_enabled = true;
    /// : qué sustituciones están encendidas, por subsistema. Todos en 1 por
    /// default — una sesión sin frontend que los maneje se comporta como antes.
    ///
    /// Vive al lado de `hd_enabled` y no lo reemplaza: aquél es la llave de luz
    /// de la casa (y además gatea la SUPRESIÓN de originales, que sin HD dejaría
    /// agujeros), y esto son las llaves de cada habitación.
    uint32_t subsystems_on = 0xFFFFFFFFu;
    /// : el último perfil que se APLICÓ. Es una pista, no la verdad — la
    /// verdad sigue siendo `subsystems_on` + los mutes, y `active_profile()`
    /// verifica esto contra ellos antes de devolverlo.
    ///
    /// Existe porque dos perfiles pueden tener el mismo efecto (uno recortado
    /// coincide con otro más chico), y ahí deducir el activo del estado
    /// devolvería cualquiera de los dos: el usuario eligió «Reinterpretado» y
    /// la UI le mostraría «Fiel». Guardar la elección lo resuelve sin
    /// introducir una segunda verdad, porque en cuanto el estado deja de
    /// coincidir la pista se descarta.
    std::string profile_hint;
    // -- Buses de audio () ----------------------------------------------
    /// Volumen por bus (índice = AudioBus). 1.0 = como se autoró.
    float bus_gain[kAudioBusCount] = {1.0f, 1.0f, 1.0f, 1.0f};
    /// Silencio por bus. Distinto de apagar el subsistema (): apagar el
    /// subsistema devuelve el ORIGINAL, silenciar el bus calla la categoría
    /// entera —HD y original— porque la intención es «no quiero música», no
    /// «prefiero la música del juego».
    bool  bus_mute[kAudioBusCount] = {false, false, false, false};
    /// : el bus DECLARADO por el pack, por firma (`audio_events.toml`).
    /// Sólo las que lo declararon: una firma ausente cae al default, que es
    /// otra cosa que «el pack dijo sin clasificar».
    std::unordered_map<uint64_t, AudioBus> audio_event_bus;

    /// El bus de un sonido, por su firma.
    ///
    /// Sale del «Tipo» que el autor le puso a la Secuencia que lo contiene. Lo
    /// que NO tiene Secuencia —una asignación por firma suelta, que hoy es el
    /// camino más usado— cae en **Efectos** (decisión de David, 2026-08-14):
    /// un sonido suelto es un efecto hasta que alguien diga lo contrario, y es
    /// la opción que no obliga a clasificar cien firmas para poder bajar la
    /// música.
    ///
    /// Una Secuencia SIN clasificar se queda en `Unclassified` y no hereda
    /// Efectos: ahí el autor tiene el control y no dijo nada; suponer sería
    /// meterle su música en el bus equivocado.
    AudioBus bus_of_signature(uint64_t sig) const {
        for (const auto& sq : audio_seq_subs) {
            if (sq.trigger_signature == sig ||
                std::find(sq.signatures.begin(), sq.signatures.end(), sig)
                    != sq.signatures.end())
                return sq.bus;
        }
        // : el bus que el PACK declara para esta firma (`audio_events.toml
        // → bus`). Va después de las Secuencias —que son más específicas— y
        // ANTES del default: un pack que clasificó sus eventos ya dijo de qué
        // son, y caer en Efectos ignoraría lo que el autor escribió.
        if (const auto it = audio_event_bus.find(sig); it != audio_event_bus.end())
            return it->second;
        return AudioBus::Sfx;
    }
    float bus_gain_of(AudioBus b) const {
        return bus_gain[static_cast<uint32_t>(b) % kAudioBusCount];
    }
    bool bus_is_muted(AudioBus b) const {
        return bus_mute[static_cast<uint32_t>(b) % kAudioBusCount];
    }
    /// ¿Se aplica el reemplazo de este subsistema en este frame? Un solo lugar
    /// donde preguntarlo: si el gate se copia en cada punto de uso, el día que
    /// se agregue una condición va a quedar puesta en la mitad de ellos.
    bool sub_on(Subsystem s) const {
        return (subsystems_on & subsystem_bit(s)) != 0;
    }
    uint8_t               plane_tile_suppress_want[3 * 1024] = {0};
    bool                  plane_tile_suppress_any = false;

    // Replay acceleration (R7d) — sin esto, replay_seek re-simulaba [0,frame)
    // desde el estado inicial en CADA llamada (scrub = O(frame), pegaba la CPU).
    //   • replay_pos: frame en el que quedó la máquina viva tras el último
    //     replay_seek (post-produce_frame); -1 = desconocido. Permite continuar
    //     hacia adelante sin unserialize.
    //   • replay_keys: keyframes (frame → savestate RAW que reproduce ese frame),
    //     capturados cada kReplayKeyInterval. Un seek arranca del más cercano ≤
    //     target, acotando el re-sim a ≤ kReplayKeyInterval frames.
    //   • replay_rec: identidad de la grabación cacheada (el caller resetea al
    //     cargar/dividir, ver replay_reset()).
    const AytherRecording*                   replay_rec = nullptr;
    int                                      replay_pos = -1;
    std::map<uint32_t, std::vector<uint8_t>> replay_keys;
    std::vector<uint8_t>                     kf_scratch;   // savestate horneado descomprimido (on-demand)

    // Keyframes horneados captados durante la grabación (crudos; se comprimen al
    // cerrar la toma en take_recording → AytherRecording::keyframes).
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> rec_keyframes;

    // Seek en chunks (R7e): reparte un seek frío (decenas de miles de frames)
    // entre frames de UI para no congelar la app. Mantiene la máquina a media
    // cadena bare entre llamadas; el resultado es idéntico a replay_seek.
    struct ChunkSeek {
        const AytherRecording* rec    = nullptr;
        bool                   active = false;
        uint32_t               target = 0;
        uint32_t               cur    = 0;   // próximo frame bare a correr
        uint32_t               start  = 0;   // origen del seek (para el progreso)
    } chunk;

    // Migración R7e (hornear keyframes en una toma vieja, troceado).
    struct Bake { const AytherRecording* rec = nullptr; bool active = false; uint32_t cur = 0; } bake;
    // Migración v8 (re-hornear la historia de hashes de sprites, troceado).
    struct HBake {
        const AytherRecording* rec    = nullptr;
        bool                   active = false;
        uint32_t               cur    = 0;
        std::vector<uint64_t>  hashes;   // CSR en construcción
        std::vector<uint32_t>  off;      // arranca {0}
    } hbake;
    bool replay_quiet = false;   // produce_frame omite la salida de audio (warm/bake)

    // Vista previa de audio (panel Capas): durante la captura los callbacks corren
    // en "modo captura" — el video no hashea (rápido) y el audio no sale al device;
    // si cap_collect, se acumula al buffer la MEZCLA de cada frame (sin filtrar por
    // hash: el audio del replay no es byte-reproducible → aislar por hash no sirve).
    bool                  cap_active  = false;
    bool                  cap_collect = false;
    std::vector<int16_t>  cap_pcm;


    // Framebuffer snapshot set by the video callback during run_frame().
    struct Snap { const void* data = nullptr; unsigned w = 0, h = 0; size_t pitch = 0; } snap;

    // Per-frame buffers (FrameView points into these; valid until next step()).
    AytherTileOccurrence   tile_occs[kMaxTileOccs];
    AytherSpriteOccurrence sprite_occs[kMaxSpriteOccs];
    AytherAudioOccurrence  audio_occs[kMaxAudioOccs];
    std::vector<AytherAudioWrite> chip_writes;  // raw FM/PSG bus writes this frame (copiado del core tras el produce)
    std::vector<AytherAudioEvent> audio_events; // eventos detectados por el último analyze_audio_events
    // : el otro camino del audio. El chip PCM de Sega CD no tiene bus
    // expuesto — llega ya tipificado por poll_audio_events — así que no puede
    // viajar en `chip_writes`. Se desempaqueta una vez por frame y entra al
    // detector en la MISMA llamada que las escrituras.
    std::vector<ayther_audio_event_v1> audio_evt_scratch;  // buffer crudo del polleo
    std::vector<AytherPcmEvent>        pcm_events;         // los de PCM, desempaquetados
    bool pcm_schema_warned = false;
    // C-A3b: sustitución por evento. Asignaciones firma→asset HD; flag de preview;
    // subs activos + máscara aplicada este produce (FrameView).
    std::unordered_map<uint64_t, std::string> audio_event_assign;
    std::unordered_map<uint64_t, uint32_t>    audio_event_channels;  // canales por firma (de la carga; fallback si no se reanalizó)
    // SECUENCIAS (Mezclar): firma disparadora → ventana relativa {duración,
    // loop}. Al rising-edge de la firma, el runtime abre una ventana de
    // range-mute de sus canales + HD (loop hasta cerrarla). 0 = sub clásica.
    std::unordered_map<uint64_t, uint32_t>    audio_event_duration;
    std::unordered_map<uint64_t, bool>        audio_event_looping;
    // : tail por firma — cuántos frames puede seguir el HD DESPUÉS de su
    // end_frame (0 = corte exacto). AUSENTE del mapa = ILIMITADO: el legacy
    // (non-loop drena entero) se conserva para lo ya autorado/horneado; las
    // entradas de Secuencia de los bakes nuevos lo escriben explícito.
    std::unordered_map<uint64_t, uint32_t>    audio_event_tail;
    static constexpr uint32_t kTailUnlimited = UINT32_MAX;
    /// : fade de fin por firma — 0 / ausente = sin fade (manda ).
    std::unordered_map<uint64_t, uint32_t>    audio_event_fade;
    uint32_t fade_of(uint64_t sig) const {
        const auto it = audio_event_fade.find(sig);
        return it == audio_event_fade.end() ? 0u : it->second;
    }
    /// : ganancia AUTORADA por firma (Secuencia). Ausente = 1.0 — el mixer
    /// aplicaba ese valor fijo, asi que un pack sin el dato suena igual.
    std::unordered_map<uint64_t, float>       audio_event_gain;
    float gain_of(uint64_t sig) const {
        const auto it = audio_event_gain.find(sig);
        return it == audio_event_gain.end() ? 1.0f : it->second;
    }
    /// : gate de condiciones — vive en el CORE (el mismo evaluador que
    /// usan los tiles). NULL cuando el pack no trae ninguna condicion, que es
    /// el caso normal: asi no se consulta nada por frame.
    struct GateDel { void operator()(AudioEventGate* g) const { ayther_audio_gate_free(g); } };
    std::unique_ptr<AudioEventGate, GateDel> audio_gate;
    /// Firmas que este frame quedaron BLOQUEADAS por sus condiciones: suenan
    /// en original. Se recalcula una vez por frame, no por disparo.
    std::unordered_set<uint64_t> audio_gate_blocked;
    bool audio_gated(uint64_t sig) const {
        return !audio_gate_blocked.empty() && audio_gate_blocked.count(sig) != 0;
    }

    /// : region de loop AUTORADA por firma, en cuadros del asset.
    /// Ausente = (0,0) = el asset entero (contrato de siempre).
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> audio_event_loop;
    std::pair<uint32_t, uint32_t> loop_of(uint64_t sig) const {
        const auto it = audio_event_loop.find(sig);
        return it == audio_event_loop.end() ? std::make_pair(0u, 0u) : it->second;
    }
    uint32_t tail_of(uint64_t sig) const {
        const auto it = audio_event_tail.find(sig);
        return it == audio_event_tail.end() ? kTailUnlimited : it->second;
    }
    /// : frame de CORTE absoluto de una ventana [.., end] según su tail
    /// (UINT64_MAX = drena entero). El MISMO número gobierna al player
    /// (EventStream.cut_frame), al barrido de one-shots y al export.
    uint64_t cut_frame_of(uint64_t sig, uint64_t end_frame) const {
        const uint32_t t = tail_of(sig);
        return t == kTailUnlimited ? UINT64_MAX : end_frame + t;
    }
    /// One-shots per-firma con tail FINITO en el aire: key → frame de corte.
    /// produce_frame los barre y corta con el fade rápido del player.
    std::unordered_map<uint64_t, uint64_t> hd_oneshot_cut;
    // : firmas MIEMBRO por Secuencia (audio_events.toml `members`) — con
    // esto la ventana mutea SOLO los eventos activos de estas firmas; sin
    // members (packs viejos) cae al range-mute de `channels`.
    std::unordered_map<uint64_t, std::vector<uint64_t>> audio_event_members;
    /// : CABEZA por firma disparadora (`head` del TOML) + paso del próximo
    /// anclaje por firma (segmentación: una ocurrencia interna no re-ancla).
    std::unordered_map<uint64_t, std::vector<uint64_t>> audio_event_head;
    std::unordered_map<uint64_t, uint64_t>              audio_event_seq_next;
    /// : las entradas de SECUENCIA del catálogo como las ve el anclaje.
    std::vector<SeqAnchorSub> audio_event_seq_view() const {
        std::vector<SeqAnchorSub> v;
        for (const auto& [sig, dur] : audio_event_duration) {
            if (!dur) continue;
            SeqAnchorSub a;
            a.key = sig; a.trigger_signature = sig; a.duration_frames = dur;
            const auto as = audio_event_assign.find(sig);
            a.enabled = as != audio_event_assign.end() && !as->second.empty();
            const auto lp = audio_event_looping.find(sig);
            a.looping = lp != audio_event_looping.end() && lp->second;
            const auto ms = audio_event_members.find(sig);
            if (ms != audio_event_members.end()) a.signatures = ms->second;
            const auto hd = audio_event_head.find(sig);
            if (hd != audio_event_head.end()) a.head = hd->second;
            v.push_back(std::move(a));
        }
        std::sort(v.begin(), v.end(), [](const SeqAnchorSub& x, const SeqAnchorSub& y) {
            return x.key < y.key;
        });
        return v;
    }
    //  F3: regla de match por asignación (opt-in; ausente = exacta legacy)
    // + índice instrumento→asignaciones para resolver una voz en O(1). El
    // índice se RECONSTRUYE al mutar asignaciones/reglas (nunca por frame).
    std::unordered_map<uint64_t, AudioMatchRuleInfo> audio_event_rule;
    AudioMatchIndex audio_match_index;

    // ---- E-2 (): suscripciones de la ABI AYTHER v1 ---------------------
    // El fork compila con el perfil ESTÁNDAR: ningún subsistema de observación
    // trabaja hasta que el frontend declare qué necesita. Los accesos legacy
    // (0x100-0x10E) funcionaban salteándose ese sistema — escribían la memoria
    // del core directo—, así que con la ABI hay que pedir explícitamente.
    //
    // Se pide `AYTHER_SUB_ALL & supported_mask` y no `ALL` a secas: el core
    // puede estar compilado sin algún subsistema, y pedir lo que no existe
    // haría fallar la llamada entera en vez de degradar.
    uint32_t ayther_subs_requested = 0;
    bool     ayther_subs_verified  = false;
    // : telemetría del juez de framebuffer (última pasada de scene_inventory).
    mutable uint32_t judge_occs = 0, judge_dropped = 0, judge_opaque = 0, judge_hits = 0;

    /// Pide las suscripciones. No-op con un core sin ABI (camino legacy).
    void activate_ayther_subscriptions() {
        if (!runner.has_ayther_v1()) return;
        const ayther_interface_v1* api = runner.ayther_api();
        if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1)) return;
        ayther_subscription_state_v1 st{};
        st.struct_size = sizeof(st);
        if (api->get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK) {
            std::fprintf(stderr,
                "[AytherSession] get_subscriptions fallo — sin suscripciones\n");
            return;
        }
        // Only what the Engine reads (docs/EMULATOR_EXTENSION_ABI.md#subscriptions).
        // AYTHER_SUB_ALL became
        // de 0x7F a 0xFFF y los bits nuevos cuestan por frame sin que nadie
        // los consuma todavía — ver RetroRunner::kEngineSubscriptions.
        const uint32_t want = RetroRunner::kEngineSubscriptions & st.supported_mask;
        const int32_t  rc   = api->set_subscriptions(want);
        if (rc != AYTHER_STATUS_OK) {
            std::fprintf(stderr,
                "[AytherSession] set_subscriptions fallo: %d\n", rc);
            return;
        }
        ayther_subs_requested = want;
        ayther_subs_verified  = false;   // se confirma tras el primer frame
        std::fprintf(stdout,
            "[AytherSession] suscripciones AYTHER pedidas: 0x%08X "
            "(soportadas: 0x%08X)\n",
            want, st.supported_mask);
    }

    // ---- E-3 (): el ESPEJO por frame de la ABI -------------------------
    // Los callers del camino por frame leían punteros VIVOS del core
    // (`video_ram()`, `color_ram()`…). Con la ABI la lectura es una copia
    // validada contra la generación del snapshot, así que el dual-path vive
    // ACÁ, en un solo lugar, y no repartido en los ~25 sitios que consumen esos
    // punteros: cada uno de esos sitios es una oportunidad de equivocarse, y el
    // objetivo de E-3 —que los bytes vengan por la ABI— se cumple igual.
    //
    // El espejo se refresca UNA vez por frame, después de `run_frame`, que es
    // cuando la ABI ya cerró su frame boundary. Sin ABI queda vacío y los
    // helpers devuelven el puntero legacy de siempre.
    ayther_frame_snapshot_v1 abi_snap{};
    bool                     abi_snap_ok = false;
    // ABI 1.5 `SYSTEM`: modo del VDP y viewport del contenido cargado, leído
    // once when creating the session (docs/EMULATOR_EXTENSION_ABI.md#observation-regions).
    // `sys_ok` = the core supplied it;
    // sin él (stock, fork viejo) se decodifican registros como siempre.
    ayther_system_v1         sys{};
    bool                     sys_ok = false;
    bool                     sys_logged = false;   ///< el primer frame con modo, una vez
    // ABI fallback semantics: two reasons deserve a dedicated warning, once
    // vez por sesión — el resto de la máscara sigue siendo «fallback» a secas.
    bool                     raster_overflow_logged  = false;
    bool                     raster_unsupported_logged = false;
    std::vector<uint8_t>     abi_vram, abi_cram, abi_regs, abi_vsram;
    std::vector<ayther_sprite_v1>      abi_sprites;
    // Cuántas entradas de `abi_sprites` son VÁLIDAS. El vector se dimensiona
    // con el count del snapshot, pero la lectura devuelve el suyo y puede ser
    // menor: usar size() como cantidad publica la cola sin llenar como si
    // fueran sprites reales (y el viewport los dibuja con patrones basura).
    // 0 = el espejo no tiene nada que ofrecer; el caller cae al legacy.
    uint32_t                           abi_sprite_count = 0;
    std::vector<ayther_audio_write_v1> abi_audio;
    bool abi_sprites_warned = false, abi_audio_warned = false;

    // Escape hatch de diagnóstico: AYTHER_ABI_MIRROR=0 apaga el espejo y deja
    // todo el dual-path cayendo al legacy. Sirve para aislar en UNA corrida si
    // una diferencia visual/de detección viene del espejo o de otro lado, con
    // el MISMO binario a los dos lados del A/B.
    static bool mirror_enabled() {
        static const bool on = [] {
            const char* v = ayther::env_get("AYTHER_ABI_MIRROR");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    void refresh_abi_mirror() {
        abi_snap_ok = false;
        abi_sprites.clear();
        abi_sprite_count = 0;
        abi_audio.clear();
        if (!mirror_enabled()) return;
        if (!runner.has_ayther_v1()) return;
        if (!runner.capture_frame_snapshot(abi_snap).ok()) return;
        // SYSTEM se refresca POR FRAME, no una vez: al crear la sesión el VDP
        // todavía no eligió modo (`vdp_mode == 0`, viewport por defecto) y
        // h40/interlace cambian con el juego. Es una lectura chica y sin
        // suscripción — se llena al leer.
        sys_ok = runner.read_system_v1(sys).ok();
        if (sys_ok && !sys_logged && sys.vdp_mode != 0) {
            sys_logged = true;
            std::fprintf(stdout,
                "[AytherSession] SYSTEM: hw=0x%02X vdp_mode=%u h40=%u interlace=%u "
                "sh=%u %s lines=%u viewport=%ux%u@(%u,%u) geometry_pending=%u\n",
                sys.system_hw, sys.vdp_mode, sys.h40, sys.interlace,
                sys.shadow_highlight, sys.region_pal ? "PAL" : "NTSC",
                sys.lines_per_frame, sys.viewport_w, sys.viewport_h,
                sys.viewport_x, sys.viewport_y,
                (unsigned)(sys.flags & AYTHER_SYSTEM_GEOMETRY_PENDING));
        }
        // El buffer se dimensiona por el MAYOR de los dos tamaños declarados:
        // `read_*_v1` escribe los bytes que dice la ABI (`query_region`), no los
        // que dice `retro_get_memory_size`. Hoy coinciden, pero dimensionar por
        // el número legacy era apostar a que sigan coincidiendo — y esa apuesta
        // se paga con un desbordamiento de heap, no con un dato raro.
        auto read_region = [&](std::vector<uint8_t>& dst, size_t n_legacy, uint32_t region,
                        RetroRunner::AytherReadResult (RetroRunner::*fn)(
                            void*, const ayther_frame_snapshot_v1&) const) {
            const size_t n_abi = runner.abi_region_bytes(region);
            const size_t n     = n_abi > n_legacy ? n_abi : n_legacy;
            if (!n) { dst.clear(); return; }
            dst.resize(n);
            const auto r = (runner.*fn)(dst.data(), abi_snap);
            // Y se publica lo LEÍDO, no lo pedido: una lectura corta dejaría la
            // cola sin llenar viajando como si fuera memoria del core (es el
            // defecto que ya mordió en los sprites del viewport, ).
            // Truncar NO alcanza: los accessors públicos (`vdp_regs(&size)` y
            // compañía) devuelven el puntero del espejo con el tamaño LEGACY,
            // así que un espejo más corto se leería de más. Una lectura corta
            // lo vuelve inservible → se cae al legacy, que sí mide lo que dice.
            if (!r.ok() || (r.count && r.count < n)) dst.clear();
        };
        read_region(abi_vram,  runner.video_ram_size(), AYTHER_REGION_VRAM,     &RetroRunner::read_vram_v1);
        read_region(abi_cram,  runner.color_ram_size(), AYTHER_REGION_CRAM,     &RetroRunner::read_cram_v1);
        read_region(abi_regs,  runner.vdp_regs_size(),  AYTHER_REGION_VDP_REGS, &RetroRunner::read_vdp_regs_v1);
        read_region(abi_vsram, runner.vsram_size(),     AYTHER_REGION_VSRAM,    &RetroRunner::read_vsram_v1);
        abi_snap_ok = true;
    }

    // Los helpers del dual-path. Si una región del espejo quedó vacía (no
    // suscripta, o el core la rechazó) se cae al puntero legacy en vez de
    // devolver nullptr: degradar a lo que funcionaba es mejor que apagar la
    // detección de sprites porque una lectura falló.
    AYTHER_LEGACY_READ_BEGIN
    const uint8_t* vram_ptr()  const {
        return !abi_vram.empty()  ? abi_vram.data()  : runner.video_ram();
    }
    const uint8_t* cram_ptr()  const {
        return !abi_cram.empty()  ? abi_cram.data()  : runner.color_ram();
    }
    const uint8_t* regs_ptr()  const {
        return !abi_regs.empty()  ? abi_regs.data()  : runner.vdp_regs();
    }
    const uint8_t* vsram_ptr() const {
        return !abi_vsram.empty() ? abi_vsram.data() : runner.vsram();
    }
    AYTHER_LEGACY_READ_END

    /// Confirma —UNA vez, tras el primer frame— que el core las activó. Es
    /// diagnóstico y no bloquea: las suscripciones entran en el frame boundary,
    /// así que preguntarlo antes de correr un frame siempre daría 0.
    void verify_ayther_subscriptions() {
        if (ayther_subs_verified || !ayther_subs_requested) return;
        if (!runner.has_ayther_v1()) return;
        const ayther_interface_v1* api = runner.ayther_api();
        if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1)) return;
        ayther_subscription_state_v1 st{};
        st.struct_size = sizeof(st);
        if (api->get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK) return;
        if (st.active_mask == ayther_subs_requested)
            std::fprintf(stdout,
                "[AytherSession] suscripciones AYTHER activas: 0x%08X\n",
                st.active_mask);
        else
            std::fprintf(stderr,
                "[AytherSession] suscripciones DESALINEADAS — activas=0x%08X "
                "pedidas=0x%08X\n", st.active_mask, ayther_subs_requested);
        ayther_subs_verified = true;
    }
    // ---- E-7 (): capas del VDP ----------------------------------------
    // Los cinco buffers viven acá y se reusan: a 320x224 son 5 x 143 KB, y
    // realocarlos por frame seria pagar un malloc por capa a 60 Hz. Orden:
    // 0=B, 1=A, 2=ventana, 3=sprites, 4=composite.
    std::vector<uint16_t> layer_bufs[5];
    void*   multilayer_fn = nullptr;        ///< el export, resuelto una vez
    bool    multilayer_fn_resolved = false;
    int32_t layers_error_status = AYTHER_STATUS_OK;
    /// El ultimo motivo YA LOGUEADO, para no repetirlo frame a frame. Distinto
    /// de `layers_error_status`, que es el de la ultima llamada: si el juego
    /// entra y sale del modo 5, cada transicion vuelve a loguear una vez.
    int32_t layers_error_logged = AYTHER_STATUS_OK;

    /// : los Acetatos que trajo el pack. La sesión los lee y los ofrece —
    /// el stack lo arma el frontend (ver pack_overlays() en el header).
    std::vector<AytherSession::PackOverlay> overlays;
    void rebuild_match_index() {
        audio_match_index.clear();
        for (const auto& [sig, r] : audio_event_rule)
            if (audio_event_assign.count(sig))
                audio_match_index.add(sig, r.rule, r.instrument, r.pitch);
    }
    ///  F3: la firma AUTORADA que cubre una voz — la exacta si está
    /// asignada, o la variante por regla (instrument == 0 nunca matchea).
    /// `sig` sigue siendo la ocurrencia real (flancos/keys); `*out` es la
    /// entrada del catálogo (asset/ventana/tail/readiness).
    bool resolve_event_sig(uint64_t sig, uint64_t instrument, uint8_t pitch,
                           uint64_t* out) const {
        if (audio_event_assign.count(sig)) {
            if (out) *out = sig;
            return true;
        }
        return audio_match_index.resolve(instrument, pitch, out);
    }
    struct SeqWindow { uint64_t end; uint32_t mask; uint64_t sig; uint64_t start = 0; };
    std::vector<SeqWindow> audio_seq_windows;
    /// : anclas de TODAS las subs de Secuencia, calculadas en UNA pasada
    /// conjunta sobre audio_events (ver seq_anchor_table). Cache por (cantidad
    /// de eventos, generación de subs): el set de subs se re-manda cada frame
    /// pero sólo cambia de generación cuando cambia de verdad.
    std::unordered_map<uint64_t, std::vector<uint32_t>> seq_anchor_cache;
    size_t   seq_anchor_for_n   = SIZE_MAX;
    uint64_t seq_anchor_for_gen = UINT64_MAX;
    uint64_t audio_seq_subs_gen = 0;
    // EM-1: cámara en espacio de NIVEL — unwrap secuencial del scroll por
    // plano (A/B). Discontinuidad de frame (seek/scrub/catch-up) re-ancla.
    int32_t  cam_x[2] = {0, 0}, cam_y[2] = {0, 0};
    int16_t  cam_prev_h[2] = {0, 0}, cam_prev_v[2] = {0, 0};
    uint64_t cam_last_frame = UINT64_MAX;
    bool     cam_valid = false;
    bool                              audio_sub_preview = false;
    // Estado del TRANSPORTE (lo setea la app por frame): los HD asignados solo
    // DISPARAN reproduciendo — al scrubear con el cabezal quieto sonaban a
    // velocidad normal, se superponían (eco) y seguían tras detenerse
    // (reporte 2026-07-23). Pausar corta lo que está en el aire.
    bool                              transport_playing = true;
    // Salida AUDIBLE (la app, por frame): false = el produce descarta su PCM
    // (cargar tomas/poses hace seeks/re-produces que no deben sonar). true por
    // default — runtime/Play y los tools no lo tocan.
    bool                              audio_audible     = true;
    /// Contadores de por qué NO sonó un frame (ver el flush en step()). Cuatro
    /// caminos que desde afuera se ven idénticos —silencio— y tienen causas y
    /// arreglos distintos. Los expone audio_health.
    uint64_t aud_n_flushed = 0, aud_n_inaudible = 0,
             aud_n_quiet   = 0, aud_n_disabled  = 0;
    // Sustitución EN VIVO (runtime, C-A4 paso 3): detector alimentado por frame +
    // máscara/firmas activas del frame anterior (1 frame de lag inherente: el
    // key-on de este frame se detecta recién tras run_frame).
    AudioEventPtr                     audio_live_det;
    bool                              audio_runtime_sub = false;
    uint32_t                          audio_runtime_mask = 0;
    std::unordered_set<uint64_t>      audio_live_prev;   // firmas activas el frame anterior
    // SECUENCIAS de autoría EN VIVO (Capturar): ventana abierta por el key-on
    // real de la firma disparadora de una sub (audio_seq_subs) sobre el
    // detector — mismo modelo que el pack exportado (range-mute de la unión de
    // canales + HD one-shot) pero con los datos de autoría (asset de disco,
    // gain). Sólo con audio_runtime_sub activo.
    struct LiveSeqWin {
        uint64_t key       = 0;   ///< id de la Secuencia (la key de su sub)
        uint64_t end_frame = 0;   ///< fin del range-mute (inclusive)
        uint64_t start_frame = 0; ///< : ancla (para el reclamo/continuación)
        uint32_t mask      = 0;   ///< canales muteados mientras la ventana vive
        uint64_t last_seen = 0;   ///< último frame con un evento miembro activo
    };
    std::vector<LiveSeqWin>                audio_live_seq_win;
    std::unordered_map<uint64_t, uint64_t> audio_live_seq_next;  // key → frame mínimo del próximo anclaje (paso = span)
    // : instancia LÓGICA por reemplazo live — separa «el evento sigue
    // activo» (esto) de «el stream está sonando» (AudioPlayer). La pausa
    // () y el bypass de Assets destruyen/omiten los streams pero
    // conservan estas instancias; reanudar las vuelve a sonar DESDE EL
    // OFFSET del reloj emulado (audio_live_resume.h) — sin esperar un
    // key-on nuevo, sin reiniciar desde cero y SIN limpiar audio_live_prev
    // (limpiarlo corrige el silencio pero mete desfase y dobles disparos).
    struct LiveInstance {
        std::string asset;                      ///< path (pack o disco)
        uint64_t    start_frame = 0;            ///< anclaje (rising-edge real)
        uint64_t    end_frame   = UINT64_MAX;   ///< fin de ventana; MAX = one-shot libre
        uint64_t    cut_frame   = UINT64_MAX;   ///< end + tail ()
        uint32_t    ev_bit      = 0;            ///< canal del evento (mute al reanudar)
        float       gain        = 1.0f;         ///< slider de la Secuencia
        bool        looping     = false;
        bool        seq_sub     = false;        ///< key = Secuencia de autoría (Capturar)
    };
    std::unordered_map<uint64_t, LiveInstance> audio_live_inst;
    /// : telemetría de la reanudación — streams re-armados con offset,
    /// instancias vencidas descartadas, y cuadros de offset acumulados
    /// (cada uno es un reinicio-desde-cero evitado).
    uint64_t hd_resumed = 0, hd_resume_finished = 0, hd_resume_offset_frames = 0;
    ///  Fase 3: Assets OFF en un workspace vivo = BYPASS — el detector y
    /// el bookkeeping (ventanas/instancias/flancos) siguen corriendo para no
    /// perder el hilo de los eventos, pero la máscara queda en 0 (suena el
    /// original) y ningún HD dispara. Volver a ON re-entra por el MISMO
    /// camino que reanudar una pausa: offset del reloj emulado.
    bool audio_live_bypass = false;
    //  Fase 4: OBSERVABILIDAD del match live. El síntoma reportado
    // (2026-08-10, transición pantalla→demo de Amazona) es «suenan originales
    // además de los assets» y desde afuera no se puede saber POR QUÉ: si son
    // colas de la escena anterior o firmas fragmentadas por el estado del
    // chip en la transición. Esto lo vuelve dato: cada firma ACTIVA se
    // clasifica por frame en exacta (asignada), variante (mismo instrumento
    // que una asignada — la fragmentación de ) o sin match, y las sin
    // match se acumulan en un registro acotado con su historia.
    //
    // El instrumento se APRENDE de los eventos CERRADOS del detector — desde
    //  F3 el FFI de actives TAMBIÉN lo trae (al key-on); lo aprendido
    // queda como memoria (armar reglas sobre firmas que ya no suenan) y
    // fallback. La nota se guarda junto al timbre (reglas kInstrumentPitch).
    struct LiveSigId { uint64_t instrument = 0; uint8_t pitch = kAudioNoPitch; };
    std::unordered_map<uint64_t, LiveSigId> live_sig_instr;    // sig → identidad
    std::unordered_set<uint64_t>            live_assigned_instr; // instr de asignadas
    struct LiveUnmatchedRec {
        uint64_t instrument   = 0;   ///< 0 = aún desconocido
        uint64_t first_frame  = 0;
        uint64_t frames_active = 0;
        uint8_t  chip = 0, channel = 0;
        bool     variant = false;    ///< mismo instrumento que una asignada
    };
    std::unordered_map<uint64_t, LiveUnmatchedRec> live_unmatched;
    static constexpr size_t kLiveUnmatchedCap = 128;
    //  F3: `live_match_rule` = frame-ocurrencias resueltas por una REGLA
    // de match (fragmentación que la regla cubrió — antes contaban `variant`).
    uint64_t live_match_exact = 0, live_match_rule = 0,
             live_match_variant = 0, live_match_none = 0;
    std::vector<AytherAudioEvent> live_evt_scratch;   // lectura de cerrados
    /// Firmas ACTIVAS del último frame que procesó el detector live. La
    /// política del router decide sus voces con esto: en vivo no hay toma
    /// alineada que consultar, y el detector corre ANTES del voice_tick en el
    /// mismo produce — el key-on que el router está decidiendo ya está acá.
    std::vector<AytherAudioActive> live_active;

    /// ¿Esta firma está CUBIERTA por la autoría? (asignación por evento, o
    /// disparadora/miembro de una Secuencia con asset)
    bool live_sig_covered(uint64_t sig) const {
        if (audio_event_assign.count(sig)) return true;
        for (const auto& sq : audio_seq_subs) {
            if (sq.asset.empty()) continue;
            if (sq.trigger_signature == sig) return true;
            if (std::find(sq.signatures.begin(), sq.signatures.end(), sig)
                    != sq.signatures.end()) return true;
        }
        return false;
    }
    uint32_t                          audio_manual_mute = 0;   // mute por canal a mano (timeline Audios)
    std::unordered_set<uint64_t>      audio_instrument_mute;   // mute DINÁMICO por instrument (panel Sonidos)
    // Mute por OCURRENCIA exacta (clave chip<<56|canal<<48|start): Secuencias
    // deshabilitadas con el ojo — ni el HD (su sub se excluye) ni el sonido
    // ORIGINAL deben oírse (reporte 2026-07-23).
    std::unordered_set<uint64_t>      audio_occurrence_mute;

    // -- RE-SÍNTESIS CON SOUNDFONT () ------------------------------------
    // Un timbre del juego (el `instrument` del detector) se reemplaza por un
    // preset de SoundFont. Es un eje COMPLEMENTARIO a la Secuencia: el juego
    // SIGUE tocando —su tempo, sus cortes— y sólo cambia el TIMBRE de una voz,
    // así que no puede desincronizar. Escalera: Secuencia > Instrumento.
    struct InstAssign {
        std::string soundfont;   ///< basename; el pack lo trae recortado
        uint16_t    bank = 0, preset = 0;
        int8_t      transpose = 0;
        float       gain = 1.0f;
    };
    std::unordered_map<uint64_t, InstAssign> inst_assign;      // instrument → preset
    /// : lo mismo, pero HORNEADO — sale de `instruments.toml` del pack en
    /// `load_pack_into`. Se mantiene aparte por el mismo criterio que
    /// `element_enhance_lab` / `element_enhance_pack` (): lo que el
    /// frontend está autorando manda sobre lo horneado, así que el del pack
    /// sólo se aplica cuando NADIE autoró (que es el caso de Play y del
    /// runtime, donde no hay quien mande el catálogo cada frame).
    std::unordered_map<uint64_t, InstAssign> inst_assign_pack;
    /// Un sintetizador POR ARCHIVO de SoundFont: cada uno se hornea recortado
    /// por su cuenta, y así un proyecto puede mezclar timbres de varios.
    /// Un sintetizador POR TIMBRE, no por archivo (). Hace falta para poder
    /// REALZAR la ganancia: CC 7 se acaba en 127, así que subir exige escalar el
    /// buffer, y eso sólo es correcto si el sintetizador atiende a una sola voz.
    /// El SoundFont parseado se comparte del lado Rust, así que N timbres del
    /// mismo archivo no lo duplican.
    std::unordered_map<uint64_t, AytherSf2*> synths;   // instrument → synth
    /// Factor de REALCE por timbre (>= 1.0). La atenuación va por CC 7; el
    /// realce, escalando el render de ESE sintetizador.
    std::unordered_map<uint64_t, float> synth_boost;
    /// Canal MIDI asignado a cada instrument. El sintetizador tiene 16 y el
    /// juego a lo sumo 10 voces, así que entran todas sin robarse notas — que
    /// es lo que pasaría mandando todo al canal 0.
    /// Notas en vuelo, para poder cerrarlas. `opened` es el frame en que
    /// arrancó: hace falta para el piso de duración (ver kSynthMinHold).
    /// `occ` es la clave de OCURRENCIA del evento que la abrió: hace falta para
    /// poder cerrarla si el artista silencia esa ocurrencia (o su canal) con la
    /// nota ya sonando — el instrumento solo no alcanza ().
    struct SynthNote { uint64_t inst, occ; int ch, key; int64_t opened, ends; bool one_shot; };
    std::vector<SynthNote> synth_on;

    /// Timbres que se disparan como ONE-SHOT (percusivos), cacheado.
    /// Se decide POR TIMBRE y no por evento: ver la nota de `synth_one_shot`.
    mutable std::unordered_map<uint64_t, bool> synth_oneshot_cache;
    mutable size_t synth_oneshot_for_n = 0;   // invalida si cambió el análisis

    /// ¿Este timbre se dispara y se deja sonar, o se sostiene y se suelta?
    ///
    /// LA DECISIÓN ES POR TIMBRE, NO POR EVENTO, y eso costó una escucha. S15
    /// de Demo Barbaro —el bombo— tiene eventos de 1 a 27 frames: con un umbral
    /// por evento, el MISMO golpe recibía dos tratamientos y sonaba «a veces
    /// piano, otros piano interrumpido». El juego suena consistente porque en
    /// FM el key-off arranca un release rápido y da casi igual cuánto se
    /// sostuvo; traducirlo a «nota de piano de 450 ms» contra «nota libre» no.
    ///
    /// El criterio: un timbre que SIEMPRE toca la misma nota es percusión —un
    /// bombo, un hi-hat, un golpe— y va como one-shot. Uno que recorre alturas
    /// es melódico y ahí sostener y soltar SÍ es parte de la interpretación.
    /// Sale de los datos, no de un campo que el artista tenga que completar.
    bool synth_one_shot(uint64_t inst) const {
        if (synth_oneshot_for_n != audio_events.size()) {
            synth_oneshot_cache.clear();
            synth_oneshot_for_n = audio_events.size();
        }
        auto it = synth_oneshot_cache.find(inst);
        if (it != synth_oneshot_cache.end()) return it->second;
        uint8_t lo = 255, hi = 0;
        for (const AytherAudioEvent& e : audio_events) {
            if (e.instrument != inst || e.pitch == 255) continue;
            if (e.pitch < lo) lo = e.pitch;
            if (e.pitch > hi) hi = e.pitch;
        }
        const bool one = (hi >= lo) && (lo == hi);
        synth_oneshot_cache[inst] = one;
        return one;
    }

    /// (Legado del umbral por evento — ya no se usa para decidir; queda el
    /// número por si hiciera falta un piso para timbres melódicos muy cortos.)
    ///
    /// Por qué. Muchos eventos del Mega Drive duran UN frame (un bombo, un
    /// golpe). Cerrarlos a los 16 ms mete el note_off dentro del ataque y no se
    /// oye nada; cerrarlos a un piso fijo los corta a mitad de la cola y suena
    /// «interrumpido». Las dos cosas se reportaron. Un piso fijo es un término
    /// medio malo: largo para el re-disparo rápido, corto para la cola.
    ///
    /// El chip tampoco cierra nada: su key-off arranca un release que sigue
    /// sonando. Un evento LARGO sí se cierra —ahí el juego sostiene la nota y
    /// soltarla es parte de la interpretación—, pero uno corto es un impacto y
    /// se deja sonar.
    ///
    /// El umbral es una heurística a ajustar de oído, no una medida.
    static constexpr int64_t kSynthOneShotMax = 4;
    std::vector<float> synth_pcm;   // buffer de render, reusado
    /// Muestras que el emulador stageó ESTE frame, capturadas ANTES del flush
    /// (que las descarta). Es la medida correcta de «cuánto audio vale este
    /// frame»; leerla después del flush daba 0 y el sintetizador caía a un
    /// número fijo que lo mataba de hambre.
    size_t  synth_frames_hint = 0;
    bool    synth_pcm_ready = false;   // synth_pcm listo para que lo sume el router
    bool  synth_any = false;        // hay al menos una asignación viva
    int64_t synth_last_frame = -1;  // último frame en que se avanzó

    /// Telemetría del sintetizador (). «Se escucha degradado, o no se
    /// escucha, y pocas veces bien» se ve IGUAL desde afuera venga de un corte
    /// por salto (seek/catch-up: apaga todas las notas), de un note_on que
    /// nunca ocurrió, de un timbre silenciado o de un frame sin PCM del
    /// emulador — y cada causa tiene un arreglo distinto. Diagnosticar eso de
    /// oído ya costó varias vueltas.
    uint64_t syn_ticks = 0,   // frames de síntesis efectivamente avanzados
             syn_jumps = 0,   // cortes por salto de frame (panic)
             syn_on    = 0,   // note_on emitidos
             syn_off   = 0,   // note_off emitidos
             syn_muted = 0,   // notas NO disparadas por timbre silenciado
             syn_nopcm = 0;   // frames sin PCM del emulador (largo estimado)

    // -- Router de canales por voz () ------------------------------------
    // PUESTO por defecto (2026-07-28): lo prende `create` llamando al setter, que
    // es donde vive la inicialización real (política, PAL, tasas del resampler y
    // el cebado del espejo). Por eso el miembro arranca en false y NO hay que
    // «corregirlo» a true acá: hacerlo saltearía todo eso — el setter no hace
    // nada si el valor ya coincide.
    bool                  voice_router_on = false;
    /// : el router SUMA su bloque al del chip en vez de ocupar su lugar.
    /// Pasa en el único hardware donde el buffer del core lleva audio que el
    /// router no espeja —el PCM RF5C164 y el CDDA del Sega CD—; ahí ocupar el
    /// lugar dejaba el sistema mudo. En cartucho el camino no cambia en nada.
    bool router_mix() const { return voice_router_on && runner.cd_media(); }
    ChannelRouter         voice_router;
    StreamResampler       voice_rs;
    int64_t               voice_last_frame = -1;
    /// Los frames cuyas escrituras todavía no consumió el router, EN ORDEN.
    /// Incluye los frames bare del catch-up: sus escrituras existen (el juego
    /// las hizo) pero produce_frame resetea el log al empezar, así que si no se
    /// capturan ahí se pierden — y el espejo se quedaría sin los key-on y los
    /// cambios de patch de hasta 31 frames.
    /// Cada entrada lleva SU frame. Con el catch-up se rinden varios frames en
    /// un tick, y la política decide por evento —que está indexado por frame de
    /// key-on—, así que pasarles a todos el frame actual elegiría mal.
    struct PendingFrame { uint32_t frame; std::vector<AytherAudioWrite> writes; };
    std::vector<PendingFrame> voice_pending;
    std::vector<float>    voice_chip;   // salida del router, a la tasa del chip
    std::vector<float>    voice_out;    // ya resampleada, a la del device
    uint64_t vr_ticks = 0, vr_primes = 0, vr_starved = 0, vr_frames = 0,
             vr_skipped = 0,   // frames cebados sin rendir (no iban a sonar)
             vr_resyncs = 0;   // veces que hubo que tirar atraso acumulado
    /// Diagnóstico del atraso. Si el router rinde MÁS audio del que el device
    /// consume, la diferencia se queda en el resampler como latencia permanente
    /// — se oiría desfasado de la imagen, no entrecortado, y por eso hay que
    /// medirlo y no deducirlo.
    size_t   vr_backlog = 0, vr_pend_max = 0;
    /// Tee de lo que el router entrega al device (AYTHER_VOICE_DUMP=<ruta>).
    /// El tee que ya existía (AYTHER_AUDIO_DUMP) saca el PCM del EMULADOR, que
    /// con el router puesto es silencio — no sirve para oír lo que se oye.
    /// Crudo f32 estéreo: sin cabecera que parchear al cerrar, así se puede
    /// levantar el archivo con el Lab todavía abierto.
    FILE*    voice_dump = nullptr;
    /// Tee del bloque del SoundFont SOLO (AYTHER_SF2_DUMP=<ruta>). Separa «no
    /// suena» de «suena bajo» de «suena y se corta» sin depender del oído, que
    /// es lo que resolvió las tres cacerías anteriores.
    FILE*    sf2_dump = nullptr;
    /// Colchón del stream del router, en muestras a 44,1 kHz. 250 ms cubre con
    /// margen los tirones de 90-150 ms que mide el probe de frame.
    static constexpr size_t kVoiceCushionFrames = 11025;   // 250 ms
    size_t   vr_queued = 0;   // lo que le queda al stream sin consumir
    double   vr_sf2_rms = 0.0, vr_sf2_peak = 0.0;

    // -- Fallback transaccional () ---------------------------------------
    /// Keys (firma de evento / key de Secuencia) cuyo ÚLTIMO intento de
    /// arranque HD falló con el asset ya decodificado (stream/bind SDL): sus
    /// ventanas dejan de silenciar el original hasta que un disparo posterior
    /// funcione. Los fallos de ASSET (missing/corrupt/etc.) no viven acá — los
    /// responde hd_ready por la cache del player, con reintento por fingerprint.
    std::unordered_set<uint64_t> hd_failed_keys;
    /// : assets distintos que fallaron, por subsistema. La clave es el
    /// índice de `Subsystem`; el valor, las RUTAS — no las firmas, porque un
    /// mismo archivo roto asignado a doce eventos es un archivo roto y no doce.
    FailureEscalation escalation;
    /// Subsistemas que el MOTOR apagó solo. Aparte de `subsystems_on` a
    /// propósito: hay que poder distinguir «el usuario lo apagó» de «se apagó
    /// por fallos», porque sólo del segundo hay algo que contarle al usuario.
    uint32_t auto_disabled_on = 0;
    /// Ocurrencias donde sonó el ORIGINAL porque el HD asignado no pudo
    /// (asset no listo o arranque fallido). El observable del fallback: crece
    /// y se oye el juego = la regla funciona; crece y hay silencio = hay un
    /// mute fuera del handshake.
    uint64_t hd_fallback = 0;

    /// ¿El asset HD de una asignación puede sonar AHORA? Decodificado y listo,
    /// del lado que le toque (pack en runtime, disco en autoría) — la MISMA
    /// elección que hace el disparo. Asignado ≠ reproducible: esta pregunta es
    /// la que autoriza a silenciar el original.
    bool hd_ready(const std::string& asset) {
        if (asset.empty()) return false;
        if (pack && ayther_pack_file_size(pack.get(), asset.c_str()) > 0)
            return audio.asset_ready_pack(pack.get(), asset);
        return audio.asset_ready_disk(asset);
    }
    /// hd_ready + sin un fallo de arranque pendiente para esta key.
    ///
    /// : el routing de audio entra POR ACÁ y no en el disparo, y esa
    /// elección es la que lo hace correcto: ésta es la pregunta ÚNICA que
    /// decide las dos mitades () — si el HD no puede sonar, el original
    /// tampoco calla. Apagar el audio HD devuelve el sonido original sin tocar
    /// ningún otro camino y sin dejar un hueco de silencio, que es exactamente
    /// lo que «restauración inmediata del contenido original» quiere decir.
    ///
    /// Música y Efectos todavía no se distinguen —eso llega con el «Tipo» de la
    /// Secuencia ()—, así que el gate es el PAR: con los dos apagados no
    /// hay audio HD. Con uno solo apagado no se puede decidir de qué bus es
    /// este sonido, y apagar el que no era sería peor que no apagar nada.
    bool hd_can_sound(uint64_t key, const std::string& asset) {
        if (!sub_on(Subsystem::Music) && !sub_on(Subsystem::Sfx)) return false;
        if (hd_failed_keys.count(key)) return false;
        if (hd_ready(asset)) return true;
        note_asset_failure(key, asset);
        return false;
    }

    // -- : ESCALADA tras fallos repetidos --------------------------------
    //
    // El fallback de  es por asset y por ocurrencia: un pack con cien
    // assets rotos reintenta cien veces, cada frame. Eso funciona —se oye el
    // juego— pero paga la resolución completa por algo que ya se sabe que no va
    // a andar, y ése es el riesgo que  anota.
    //
    // Se cuentan ASSETS DISTINTOS, no ocurrencias. Un archivo roto que suena mil
    // veces es UN problema; doce archivos distintos es un pack mal armado o una
    // carpeta que no llegó. Contar ocurrencias apagaría el subsistema por un
    // solo asset que se repite mucho, que es justo el caso que NO hay que
    // castigar.
    void note_asset_failure(uint64_t key, const std::string& asset) {
        if (asset.empty()) return;
        // El subsistema sale del BUS de la firma (): un asset de música que
        // falta no dice nada sobre los efectos, y apagar los dos por uno sería
        // llevarse puesto lo que sí funciona.
        const Subsystem sub = (bus_of_signature(key) == AudioBus::Music)
                                  ? Subsystem::Music : Subsystem::Sfx;
        const uint32_t si    = static_cast<uint32_t>(sub);
        const size_t previous_count = escalation.count(si);
        const bool     cruzo = escalation.note(si, asset);
        // Sólo la PRIMERA vez que este asset falla: el mismo archivo se
        // reintenta cada frame, y loguearlo cada vez haría que el registro
        // creciera sin decir nada nuevo.
        if (escalation.count(si) == previous_count) return;

        // El PACK en el registro (): el asset y la causa ya estaban, pero
        // de qué pack venía, no — y con los nombres por hash () eso es lo
        // único que permite volver al proyecto que lo horneó.
        std::fprintf(stderr, "[degradación] %s no se pudo reproducir (pack: %s) "
                             "— %zu/%zu del subsistema\n",
                     asset.c_str(),
                     pack_path.empty() ? "(sin pack)" : pack_path.c_str(),
                     escalation.count(si), escalation.threshold());

        if (!cruzo) return;
        if (!sub_on(sub)) return;   // ya estaba apagado: nada que escalar
        subsystems_on   &= ~subsystem_bit(sub);
        auto_disabled_on |= subsystem_bit(sub);
        std::fprintf(stderr, "[degradación] %s APAGADO tras %zu archivos que no "
                             "se pudieron reproducir — se sigue con el original\n",
                     sub == Subsystem::Music ? "música HD" : "efectos HD",
                     escalation.count(si));
    }

    /// : ¿este sonido está silenciado por su BUS? Se pregunta por FIRMA,
    /// que es la identidad que tienen las dos mitades — el evento original y su
    /// reemplazo—, y por eso alcanza a las dos con una sola respuesta: la misma
    /// disciplina de .
    ///
    /// OJO con la asimetría respecto de , que es deliberada: silenciar un
    /// bus NO devuelve el original (eso es apagar el subsistema). Silenciar el
    /// bus de Música quiere decir «no quiero música», así que se va también la
    /// del juego.
    bool bus_muted_for(uint64_t sig) const {
        return bus_is_muted(bus_of_signature(sig));
    }
    /// Registrar el resultado de un disparo HD: un fallo deja de silenciar el
    /// original desde el frame siguiente (el router lo ve en ESTE mismo frame);
    /// un éxito re-arma el mute de la key. Devuelve `played` para encadenar.
    bool hd_fired(uint64_t key, bool played) {
        if (played) hd_failed_keys.erase(key);
        else        { hd_failed_keys.insert(key); ++hd_fallback; }
        return played;
    }

    /// : re-armar los reemplazos live tras una pausa (o al volver del
    /// bypass de Assets). NO toca audio_live_prev ni las ventanas: la
    /// instancia lógica sobrevivió al corte físico () y el stream se
    /// recrea desde el offset del reloj emulado — un evento sostenido
    /// vuelve a sonar sin esperar un key-on nuevo, un loop conserva su
    /// fase (módulo en el player) y una instancia vencida se descarta en
    /// vez de reiniciarse desde cero.
    void resume_live_instances() {
        if (!audio_enabled || !transport_playing || audio_live_bypass ||
            audio_live_inst.empty())
            return;
        const double fps = runner.fps();
        for (auto it = audio_live_inst.begin(); it != audio_live_inst.end(); ) {
            const uint64_t      key = it->first;
            const LiveInstance& li  = it->second;
            // Silenciado por el artista: la instancia sigue viva (el mute es
            // intención, no fin del evento) pero no suena.
            if (li.seq_sub) {
                const AudioSeqSub* sq = nullptr;
                for (const auto& s : audio_seq_subs)
                    if (s.key == key) { sq = &s; break; }
                if (!sq) { it = audio_live_inst.erase(it); continue; }  // sub quitada
                if (seq_sub_muted_live(*sq)) { ++it; continue; }
            } else if (signature_muted(key, li.ev_bit)) {
                ++it; continue;
            }
            // : asignado ≠ reproducible — con el asset roto/ausente suena
            // el original (su ventana ya no mutea); si el archivo aparece,
            // una próxima reanudación lo levanta.
            if (!hd_can_sound(key, li.asset)) { ++it; continue; }
            const bool in_pack =
                pack && ayther_pack_file_size(pack.get(), li.asset.c_str()) > 0;
            // La duración poda un one-shot ya drenado; para el pack la poda
            // la hace el player (offset pasado el final = éxito sin stream).
            const double dur = in_pack ? 0.0
                                       : audio.asset_duration_seconds(li.asset);
            const auto d = ayther::live_resume_decide(
                frame_index, li.start_frame, li.end_frame, li.cut_frame,
                li.looping, fps, dur);
            if (d.action == ayther::LiveResumeAction::Finished) {
                ++hd_resume_finished;
                it = audio_live_inst.erase(it);
                continue;
            }
            const bool played = in_pack
                // : la rama de disco ya respetaba `li.gain` y la del pack
                // no — el MISMO audio reanudado sonaba distinto segun de donde
                // saliera. El fade queda en 0 explicito: lo de esta rama es el
                // gain, no cambiar el contrato de fin.
                ? audio.play_event_hd(pack.get(), li.asset.c_str(), li.looping,
                                      key, li.end_frame, li.cut_frame,
                                      d.offset_seconds, 0u, li.gain)
                : audio.play_oneshot_asset_file(li.asset, key,
                                                d.offset_seconds, li.gain);
            if (hd_fired(key, played)) {
                ++hd_resumed;
                hd_resume_offset_frames += frame_index - li.start_frame;
                // : el one-shot de disco con tail finito vuelve a entrar
                // al barrido (la pausa pudo cruzar un toggle que lo limpió).
                if (!in_pack && !li.seq_sub && li.cut_frame != UINT64_MAX)
                    hd_oneshot_cut[key] = li.cut_frame;
            }
            ++it;
        }
    }

    /// : range-mute del catálogo del pack, transaccional — reemplaza al
    /// mute_at plano de AudioEventSubstitution. El frame se descarta solo si
    /// alguna sustitución que lo cubre está silenciada por el artista
    /// (intención) o tiene su asset del pack LISTO y sin fallo de arranque:
    /// un asset roto en el pack ya no deja su ventana en silencio.
    bool pack_evt_mute_at(uint64_t f) {
        const AytherAudioEventSub* s = audio_evt.subs();
        const uint32_t n = audio_evt.sub_count();
        for (uint32_t i = 0; i < n; ++i) {
            if (f < s[i].start_frame || f > s[i].end_frame) continue;
            if (signature_muted(s[i].signature, 0)) return true;
            if (!hd_failed_keys.count(s[i].signature) && pack &&
                audio.asset_ready_pack(pack.get(), s[i].asset_path))
                return true;
        }
        return false;
    }

    /// ¿Este evento YA tiene quien lo reemplace? Si lo tiene, su voz original
    /// no debe sonar — y ésa es toda la diferencia entre el modelo viejo y éste:
    /// antes había que ACERTAR una ventana de mute, ahora la voz simplemente no
    /// emite desde su propio key-on hasta el fin de su cola.
    ///
    /// : «lo tiene» exige el asset LISTO (hd_can_sound), no la mera
    /// asignación — con el asset roto la voz original vuelve a sonar desde su
    /// propio key-on (fallback transaccional). No-const: la consulta de
    /// readiness puede decodificar/re-statear la cache del player.
    bool voice_replaced(const AytherAudioEvent& e) {
        // Silenciado por el artista, por cualquiera de los tres ejes ().
        // Con el router puesto el chip calla ENTERO, así que las máscaras del
        // camino viejo (occurrence_mute_at, audio_manual_mute) ya no lo tocan:
        // si esta pregunta no las incluyera, el ojo de una Secuencia y el mute
        // por canal dejarían de silenciar nada.
        if (event_muted(e)) return true;
        // Sustituido por un asset HD, por evento o por Secuencia — esos suenan
        // por su propio camino (play_event_hd / play_oneshot_asset_file), así
        // que copiar el original acá sería oír los dos juntos. Eso era el
        // «suena degradado» del 2026-07-28. Solo si el asset PUEDE sonar
        // () — roto/ausente, la voz copia el original y se oye el juego.
        //  Fase 3: en bypass de Assets el reemplazo HD queda en suspenso
        // y la voz ORIGINAL suena — sin este gate, el router seguía callando
        // las voces asignadas con el toggle Assets apagado. La re-síntesis
        // SoundFont (abajo) no entra en el bypass: su synth sigue tocando.
        if (!audio_live_bypass) {
            //  F3: la asignación se resuelve exacta O por regla de
            // instrumento — la voz variante (misma voz en otra nota/canal)
            // también calla; su HD sale por el mismo camino que la exacta.
            uint64_t asig = 0;
            if (resolve_event_sig(e.signature, e.instrument, e.pitch, &asig))
                if (const auto it = audio_event_assign.find(asig);
                    it != audio_event_assign.end() &&
                    hd_can_sound(asig, it->second)) return true;
            for (const auto& sq : audio_seq_subs) {
                if (sq.asset.empty()) continue;
                if (std::find(sq.signatures.begin(), sq.signatures.end(),
                              e.signature) != sq.signatures.end() &&
                    hd_can_sound(sq.key, sq.asset)) return true;
            }
        }
        // Re-sintetizado con SoundFont (): su voz original calla. El DAC y
        // el ruido no aplican — ahí el SF2 no tiene nota que tocar.
        if (e.pitch != 255 && inst_assign.count(e.instrument)) return true;
        return false;
    }

    /// La pregunta de voice_replaced() para una voz EN VIVO (Capturar y el
    /// runtime del pack): no hay toma que alinear, así que la identidad viene
    /// del detector — la firma que le puso a este canal ESTE frame. Las
    /// preguntas son las mismas del replay: asignación resuelta exacta o por
    /// regla ( F3), ventanas per-firma con members () y ventanas de
    /// Secuencia de autoría, cada una con su gate transaccional () y el
    /// silencio del artista alcanzando al original ().
    /// : la FIRMA activa en este canal, o 0 si el detector no la ve. Es lo
    /// que deja saber de qué bus es la voz para escalarla — la misma pregunta
    /// que ya responde `bus_of_signature`, con la misma respuesta.
    uint64_t live_voice_signature(const VoiceContext& ctx) const {
        for (const auto& c : live_active)
            if (c.chip == ctx.chip && c.channel == ctx.channel) return c.signature;
        return 0;
    }

    bool live_voice_replaced(const VoiceContext& ctx) {
        const AytherAudioActive* a = nullptr;
        for (const auto& c : live_active)
            if (c.chip == ctx.chip && c.channel == ctx.channel) { a = &c; break; }
        // Voz que el detector no ve (residual, ajena): suena tal cual —
        // de más y no de menos, igual que el camino de replay.
        if (!a) return false;
        // El altavoz del Patrón silencia el timbre también en vivo — y antes
        // del bypass, como event_muted() en voice_replaced().
        if (a->instrument && audio_instrument_mute.count(a->instrument))
            return true;
        // : y el bus, por el mismo motivo y en el mismo lugar — un bus
        // silenciado calla la voz esté o no sustituida.
        if (bus_muted_for(a->signature)) return true;
        //  Fase 3: bypass de Assets = el juego original entero.
        if (audio_live_bypass) return false;
        const uint64_t sig = a->signature;
        // Asignación por evento (per-firma / packs), exacta o por regla.
        uint64_t asig = 0;
        if (resolve_event_sig(sig, a->instrument, a->pitch, &asig))
            if (const auto it = audio_event_assign.find(asig);
                it != audio_event_assign.end() &&
                hd_can_sound(asig, it->second)) return true;
        // Ventanas per-firma vivas (packs: [[event]] con duration/members).
        for (const auto& w : audio_seq_windows) {
            if (frame_index > w.end) continue;
            const auto wit = audio_event_assign.find(w.sig);
            if (wit == audio_event_assign.end() ||
                !hd_can_sound(w.sig, wit->second)) continue;   // 
            const auto mit = audio_event_members.find(w.sig);
            if (mit == audio_event_members.end() || mit->second.empty()) {
                // Packs viejos sin members: range-mute de la máscara.
                if (w.mask & chan_bit(ctx.chip, ctx.channel)) return true;
            } else if (sig == w.sig ||
                       std::find(mit->second.begin(), mit->second.end(), sig)
                           != mit->second.end()) {
                return true;
            }
        }
        // Ventanas de Secuencia de autoría (Lab): miembro exacto, disparadora,
        // o el timbre del disparador bajo regla ( F3 — la voz variante de
        // la transición calla junto con las exactas).
        for (const auto& w : audio_live_seq_win) {
            const AudioSeqSub* sq = nullptr;
            for (const auto& s : audio_seq_subs)
                if (s.key == w.key) { sq = &s; break; }
            if (!sq) continue;
            const bool member =
                sig == sq->trigger_signature ||
                std::find(sq->signatures.begin(), sq->signatures.end(), sig)
                    != sq->signatures.end() ||
                (sq->match_rule != AudioMatchRule::kExact &&
                 sq->match_instrument && a->instrument &&
                 a->instrument == sq->match_instrument);
            if (!member) continue;
            // El ojo silencia también al original (); sin HD sano el
            // original debe sonar ().
            if (seq_sub_muted_live(*sq) || hd_can_sound(w.key, sq->asset))
                return true;
        }
        // El SoundFont () no entra acá: su re-síntesis toca desde los
        // eventos analizados de la toma — en vivo no hay nota que reponer,
        // y callar el original sin reemplazo sería un mudo.
        return false;
    }

    /// La política del router. Ata lo que el router sabe —CUÁNDO arranca una voz
    /// y en qué canal— con lo que el artista asignó, que está indexado por firma
    /// y por instrumento. El puente son los eventos ya analizados.
    struct SessionPolicy final : VoicePolicy {
        Impl*        im = nullptr;
        SilentSource silent;
        /// : una fuente con ganancia POR BUS, no por voz. El factor se lee
        /// por render —apunta al array de la sesión— así que una sola alcanza
        /// para todas las voces de ese bus y no hay nada que sincronizar
        /// cuando el usuario mueve el slider.
        std::vector<GainSource> gains;

        /// Lo que suena cuando la voz NO está reemplazada: la copia de siempre,
        /// o la copia ESCALADA si su bus tiene el volumen bajo.
        ///
        /// nullptr sigue significando «copia tal cual», que es el contrato del
        /// router: el caso normal no paga ni una indirección.
        IVoiceSource* pass(uint64_t sig) {
            if (!sig) return nullptr;   // el detector no la ve: de más, no de menos
            const uint32_t b = static_cast<uint32_t>(im->bus_of_signature(sig));
            if (b >= kAudioBusCount) return nullptr;
            if (im->bus_gain[b] == 1.0f) return nullptr;
            return &gains[b];
        }

        IVoiceSource* choose(const VoiceContext& ctx) override {
            if (!im) return nullptr;
            // Mute por CANAL: incondicional, TODO el frame — ése es su
            // contrato, y por eso va ANTES del análisis. Con el chequeo sólo
            // dentro del loop de eventos, silenciar un canal para auditarlo
            // dejaba sonando todo key-on que el análisis no conociera.
            if (im->audio_manual_mute & chan_bit(ctx.chip, ctx.channel))
                return &silent;
            // EN VIVO (Capturar / runtime del pack): frame_index no alinea
            // con la toma analizada — el loop de abajo no encontraría el
            // evento (y una coincidencia numérica sería un match espurio de
            // otra escena). La verdad en vivo es el detector. Sin esta rama,
            // con el router puesto (default) el range-mute live moría en
            // `audio_mute = 0` y el original entero sonaba DEBAJO de los HD —
            // el «se cuelan originales además de los assets» de la transición
            // pantalla→demo (reporte 2026-08-10).
            if (im->audio_runtime_sub)
                return im->live_voice_replaced(ctx)
                           ? static_cast<IVoiceSource*>(&silent)
                           : pass(im->live_voice_signature(ctx));
            for (const AytherAudioEvent& e : im->audio_events) {
                if (e.start_frame != ctx.frame) continue;
                if (e.chip != ctx.chip || e.channel != ctx.channel) continue;
                return im->voice_replaced(e)
                           ? static_cast<IVoiceSource*>(&silent)
                           : pass(e.signature);
            }
            // Un key-on que el análisis no conoce (residual, o la toma sin
            // analizar) suena tal cual: de más y no de menos.
            return nullptr;
        }
    };
    SessionPolicy voice_policy;

    /// ¿El audio de este frame va a salir por el device? Es la MISMA compuerta
    /// que abre el flush; el router tiene que mirarla, no suponerla.
    bool voice_audible() const {
        return audio_enabled && !replay_quiet && audio_audible;
    }

    /// Guarda las escrituras de un frame para que el router las consuma.
    void voice_capture(const AytherAudioWrite* w, uint32_t n, uint32_t frame) {
        if (!voice_router_on) return;
        // : el playback secuencial extiende el caché de cebado gratis
        // (solo si es el frame contiguo a lo construido y de la toma dueña).
        if (replay_rec && voice_prime_rec == static_cast<const void*>(replay_rec))
            voice_prime_push(frame, w, n);
        // Un frame que NO va a sonar —el análisis de la toma re-emula 2000 y
        // pico, y los produce internos de la app agregan más— no aporta audio,
        // pero su ESTADO sí importa. Se ceba en el acto en vez de encolarse.
        //
        // Encolarlos era el bug del «suena degradado»: se acumulaban hasta el
        // tope, el primer flush rendía DOS SEGUNDOS de audio de una y, como el
        // device sólo consume un frame por vez, el resto se quedaba en el
        // resampler para siempre. Medido: atraso=94194 muestras = 2,14 s de
        // desfase con la imagen, y la cola en 129.
        if (!voice_audible()) {
            voice_router.mirror().prime_frame(w, n);
            voice_last_frame = (int64_t)frame_index;
            ++vr_skipped;
            return;
        }
        voice_pending.push_back({ frame, std::vector<AytherAudioWrite>(w, w + n) });
    }

    /// Corta TODO y vacía lo encolado. Para los seeks y los cortes de escena:
    /// sin esto quedan notas sonando sobre lo que sigue.
    void synth_panic() {
        for (auto& [_, s] : synths) ayther_sf2_all_notes_off(s);
        synth_on.clear();
        audio.clear_synth();
        synth_last_frame = -1;
    }

    /// Un frame de síntesis. Se llama SÓLO desde el flush de audio, o sea sólo
    /// cuando el PCM del emulador también sale — así el timbre no se adelanta
    /// ni queda sonando durante un produce interno.
    void synth_tick() {
        synth_pcm_ready = false;
        // Mismo gate que el muteo: con los Assets apagados no suena.
        if (!synth_any || !audio_sub_preview || audio_events.empty()) {
            if (!synth_on.empty()) synth_panic();
            return;
        }
        const int64_t f = (int64_t)frame_index;

        // Un salto ATRÁS, o uno más largo que el catch-up, es un seek o un
        // scrub: ahí hay que cortar o las notas del tramo viejo suenan sobre el
        // nuevo. Pero avanzar VARIOS frames de una no es un salto: es el
        // catch-up normal del playback. El Lab no corre a 16,7 ms por frame, así
        // que produce_frame saltea frames que igual se emulan en bare y cuyo PCM
        // VA AL DEVICE.
        //
        // Tratar eso como seek era catastrófico y por eso está medido: 490
        // panics en 707 frames de reproducción, o sea que casi toda nota se
        // apagaba a los pocos ms de arrancar. Ese era el «se escucha degradado,
        // o no se escucha, y pocas veces bien» (reporte 2026-07-27) — y no se
        // parecía en nada a lo que yo venía buscando adentro del sintetizador.
        constexpr int64_t kSynthCatchUpMax = 32;   // = kFastForwardMax del replay

        // Re-producir el MISMO frame no avanza nada, y este chequeo va PRIMERO.
        // produce_frame no es 1:1 con los frames emulados (compose,
        // replay_invalidate, export), así que repetir es lo NORMAL — y con el
        // chequeo después del de salto, cada repetición entraba por
        // `f <= synth_last_frame` y hacía panic, o sea que apagaba TODAS las
        // notas en vuelo. Ése era el «cuando asigno un SF2 a un sonido no se oye
        // bien» (reporte 2026-07-28): el timbre arrancaba y se cortaba enseguida.
        // Mismo bug que ya había aparecido en voice_tick, misma corrección.
        if (f == synth_last_frame) return;

        if (synth_last_frame >= 0 &&
            (f < synth_last_frame || f > synth_last_frame + kSynthCatchUpMax)) {
            ++syn_jumps;
            synth_panic();
        }
        const int64_t from = synth_last_frame < 0 ? f : synth_last_frame + 1;
        synth_last_frame = f;
        ++syn_ticks;

        // CUÁNTAS MUESTRAS. No 44100/fps: EXACTAMENTE las que el emulador
        // stageó, que con catch-up son las de VARIOS frames juntos.
        //
        // Un número fijo asume que el Lab corre a tiempo real, y no lo hace —
        // el probe de frame mide 20-33 ms donde el ideal es 16,7. Con fijo, el
        // stream del sintetizador recibe de menos y se muere de hambre (audio
        // entrecortado), mientras el del emulador se salva porque tiene DRC que
        // estira su ritmo. Atados al mismo número, los dos derivan igual y no
        // se separan nunca.
        //
        // El fallback sólo cubre el frame raro sin PCM del emulador.
        const double fps = runner.fps() > 1.0 ? runner.fps() : 60.0;
        size_t n = synth_frames_hint;
        if (n == 0) { ++syn_nopcm; n = (size_t)(44100.0 / fps + 0.5); }
        synth_pcm.assign(n * 2, 0.0f);

        // Los frames salteados por el catch-up SE PROCESAN igual —sus key-on y
        // key-off— y el render se reparte entre ellos. Aplicarlos todos juntos
        // al principio del bloque alcanzaría para que suenen, pero amontonaría
        // hasta medio segundo de notas en un instante; repartir mantiene el
        // orden y el tiempo aproximado.
        const int64_t span = f - from + 1;
        size_t done = 0;
        for (int64_t g = from; g <= f; ++g) {
            // Cierres primero: dos notas contiguas comparten frontera y abrir
            // antes de cerrar dejaría la vieja colgada.
            for (size_t i = synth_on.size(); i-- > 0;) {
                const SynthNote& sn = synth_on[i];
                // One-shot: no se cierra solo. Lo corta el re-disparo de abajo,
                // el corte global de un seek, o el mute de acá.
                //
                // El artista silenció este sonido (): su voz sintetizada
                // calla igual que la del chip. Incluye el one-shot EN VUELO —
                // si no se cierra acá no se cierra nunca, y seguía sonando
                // sobre el mute.
                //
                // Por OCURRENCIA y no sólo por timbre (): el ojo de una
                // Secuencia y el mute por canal también son «silenciar», y con
                // el chequeo por instrumento nada más el SoundFont seguía
                // tocando la ocurrencia que el artista acababa de apagar.
                const bool muted = occurrence_muted(sn.occ, sn.inst);
                if (sn.one_shot && !muted) continue;
                // Cierra cuando termina ESTA NOTA, no cuando el instrumento
                // deja de sonar. Preguntar por el instrumento era el bug del
                // «después se reproduce casi continuo sostenido sin variar el
                // tono» (reporte 2026-07-28): entre dos notas seguidas del mismo
                // timbre SIEMPRE hay un evento cubriendo el frame, así que la
                // primera nunca se cerraba y la siguiente sumaba una voz más.
                // Se apilaban hasta volverse un acorde sostenido donde ya no se
                // distingue la melodía. El chip hace exactamente esto: cada
                // evento es un key-on y su key-off.
                if (!muted && g <= sn.ends) continue;
                auto it = inst_assign.find(sn.inst);
                if (it != inst_assign.end()) {
                    if (AytherSf2* sy = synth_for(sn.inst))
                        { ayther_sf2_note_off(sy, sn.ch, sn.key); ++syn_off; }
                }
                synth_on.erase(synth_on.begin() + (long)i);
            }

            for (const AytherAudioEvent& e : audio_events) {
                if ((int64_t)e.start_frame != g) continue;
                if (e.pitch == 255) continue;          // DAC/ruido: no hay nota
                auto it = inst_assign.find(e.instrument);
                if (it == inst_assign.end()) continue;
                // Silenciar un sonido lo silencia ENTERO (, ). Antes el
                // mute sólo llegaba al chip y el SoundFont seguía tocando: el
                // panel decía «silenciado» y se oía igual. Los tres ejes, no
                // sólo el instrumento — ver event_muted.
                //
                // El chequeo va DESPUÉS del inst_assign para que `syn_muted`
                // cuente lo que dice contar: notas sintetizadas que no sonaron
                // por el mute. Antes iba primero y sumaba también los timbres
                // sin SoundFont, que no habrían sonado igual — el contador
                // marcaba 301 con una sola asignación viva.
                if (event_muted(e)) { ++syn_muted; continue; }
                // Prioridad ASSET > SF2: si un asset (por firma o por
                // Secuencia activa) ya toca este evento, el SoundFont calla —
                // superpuestos era el reporte 2026-07-31.
                if (event_covered_by_asset(e, (uint32_t)g)) continue;
                AytherSf2* sy = synth_for(e.instrument);
                if (!sy) continue;
                // Canal 0 SIEMPRE: cada timbre tiene su propio sintetizador, así
                // que ya no hay que repartirlos entre canales MIDI.
                const int ch = 0;
                int key = (int)e.pitch + it->second.transpose;
                if (key < 0 || key > 127) continue;    // fuera de rango: se descarta
                const int vel = e.velocity ? (int)e.velocity : 100;
                // Re-disparo de la MISMA nota que sigue en vuelo (un golpe que
                // se repite rápido): se cierra primero, o el sintetizador
                // acumula voces del mismo tono que se suman y saturan.
                for (size_t i = synth_on.size(); i-- > 0;) {
                    if (synth_on[i].ch != ch || synth_on[i].key != key) continue;
                    ayther_sf2_note_off(sy, ch, key);
                    ++syn_off;
                    synth_on.erase(synth_on.begin() + (long)i);
                }
                // Ganancia del timbre (): CC 7 del canal, ANTES del
                // note_on, y no una sola vez al asignar: el artista la mueve
                // con el slider y el cambio tiene que oírse sin re-asignar.
                //
                // 1.0 → 127, o sea NEUTRO: el timbre entra al nivel que tiene el
                // SoundFont, y desde ahí se baja. Antes 1.0 mandaba 100, que ya
                // es −4 dB de arranque; con el A/B no se notaba porque el timbre
                // sonaba solo, pero conviviendo con la mezcla del router quedaba
                // 18 dB por debajo del juego (medido con AYTHER_SF2_DUMP:
                // −45 dBFS contra −27). Un default no debería atenuar.
                // La ganancia se parte en dos porque MIDI no puede realzar:
                // hasta 1.0 baja por CC 7 (0-127), y por encima sube escalando
                // el render de ESTE sintetizador — exacto, porque atiende a una
                // sola voz (). 1.0 es el centro: ni atenúa ni realza.
                const float gn = it->second.gain;
                ayther_sf2_control(sy, ch, 7,
                    std::clamp(static_cast<int>(std::min<float>(gn, 1.0f) * 127.0f + 0.5f),
                               0, 127));
                const bool one_shot = synth_one_shot(e.instrument);
                ayther_sf2_note_on(sy, ch, key, vel);
                ++syn_on;
                synth_on.push_back({ e.instrument,
                                     occ_key(e.chip, e.channel, e.start_frame),
                                     ch, key, g, (int64_t)e.end_frame, one_shot });
            }

            const size_t upto = std::min<size_t>(
                n, (size_t)((double)n * (double)(g - from + 1) / (double)span));
            if (upto <= done) continue;
            const size_t chunk = upto - done;
            for (auto& [inst, sy] : synths) {
                // Un sintetizador por TIMBRE: se renderizan todos y se suman,
                // cada uno con SU realce. Escalar acá es exacto justamente
                // porque el sintetizador atiende a una sola voz ().
                static std::vector<float> tmp;
                tmp.assign(chunk * 2, 0.0f);
                ayther_sf2_render(sy, tmp.data(), chunk);
                const auto bit = synth_boost.find(inst);
                const float g = bit == synth_boost.end() ? 1.0f : bit->second;
                for (size_t i = 0; i < tmp.size(); ++i)
                    synth_pcm[done * 2 + i] += tmp[i] * g;
            }
            done = upto;
        }
        // Con el router puesto NO se encola acá: los dos alimentan el mismo
        // stream y encolar de a dos le daría el doble de muestras por frame. Se
        // deja listo y voice_tick lo SUMA a su mezcla — los dos producen
        // exactamente `n` muestras, así que sumar es trivial y queda un solo
        // camino de salida.
        if (voice_router_on) { synth_pcm_ready = true; return; }
        audio.feed_synth(synth_pcm.data(), n);
    }

    /// Un frame del router de voces (). Se llama desde el MISMO lugar que
    /// synth_tick —el flush de audio— por el mismo motivo: sólo cuando el PCM
    /// del emulador también sale, o el audio se adelanta durante un produce
    /// interno.
    ///
    /// Devuelve si dejó SU bloque en el staging del player. Importa: el router
    /// no silencia al chip, le OCUPA EL LUGAR (buffer_router pisa el PCM
    /// staged), así que un frame en el que el router no rinde es un frame en el
    /// que lo staged sigue siendo del chip — y hay que tirarlo, no empujarlo.
    bool voice_tick() {
        if (!voice_router_on) return false;
        const int64_t f = (int64_t)frame_index;

        // Un salto ATRÁS, o uno más largo que el catch-up, es un seek. Esta es
        // la lección cara de synth_tick: avanzar VARIOS frames de una NO es un
        // salto, es el catch-up normal — el Lab no corre a 16,7 ms por frame.
        // Tratarlo como seek daba 490 panics en 707 frames de reproducción.
        constexpr int64_t kVoiceCatchUpMax = 32;   // = kFastForwardMax del replay

        // Re-producir el MISMO frame no avanza nada, y este chequeo va PRIMERO.
        // Con él después del de salto, una pausa cebaba la toma entera en CADA
        // frame —f <= voice_last_frame es cierto al repetir— y el frame se iba a
        // 100 ms. produce_frame no es 1:1 con los frames emulados (compose,
        // replay_invalidate, export), así que repetir es lo normal, no la
        // excepción.
        if (f == voice_last_frame) { voice_pending.clear(); return false; }

        // : llegar SIN historia (voice_last_frame < 0 — sesion nueva o
        // despues de replay_invalidate) a un frame que no es el 0 es el salto
        // MAS frio que existe, y sin embargo no contaba como salto: la
        // condicion exigia `>= 0`, asi que el espejo se quedaba sin cebar y
        // sintetizaba con los registros en reset. Eso es el audio DELGADO del
        // reporte — medido con tools/audio_seek_probe: -74,3 % de nivel.
        //
        // Es la misma clase de defecto que el espejo mudo al arranque de la
        // toma: el caso «no hay estado previo» se leia como «no hace falta
        // reponer nada», cuando es exactamente al reves.
        const bool jumped = (voice_last_frame < 0 && f > 0) ||
                            (voice_last_frame >= 0 &&
                             (f < voice_last_frame ||
                              f > voice_last_frame + kVoiceCatchUpMax));
        if (jumped) {
            // A diferencia del SoundFont, acá no alcanza con callar las notas:
            // el espejo necesita el ESTADO DE REGISTROS del punto al que se
            // saltó, o el timbre sale mal (Fase 0: la correlación de envolvente
            // cae de 0,975 a 0,889 arrancando en frío). Se reconstruye
            // recorriendo las escrituras de la toma sin generar audio.
            //
            // Lo pendiente de ESTE frame SOBREVIVE al cebado (). El cebado
            // reconstruye [0, f) desde la toma y después limpia la cola — pero
            // el produce que está corriendo ya capturó las escrituras del frame
            // f, que son las únicas que el cebado NO cubre. Tirarlas dejaba al
            // router sin el key-on del frame al que se saltó: la voz no
            // arrancaba, la política nunca se consultaba (por eso no callaba
            // nada) y el canal quedaba mudo hasta el próximo key-on. Se notaba
            // justo donde más importa —saltar al inicio de una Secuencia, que
            // es el frame del key-on— y en el replay de una toma recién
            // analizada, donde el análisis deja el cursor al final y el primer
            // frame reproducido es siempre un salto.
            std::vector<PendingFrame> keep;
            for (auto& pf : voice_pending)
                if ((int64_t)pf.frame >= f) keep.push_back(std::move(pf));
            voice_prime_to((uint32_t)f);
            voice_pending = std::move(keep);
        }
        voice_last_frame = f;
        ++vr_ticks;

        // Rendir TODOS los frames pendientes —los bare del catch-up incluidos—
        // a la tasa del chip, y recién después convertir de una.
        // CUÁNTAS MUESTRAS: exactamente las que stageó el emulador, igual que el
        // sintetizador. Atados al mismo número, los dos streams derivan igual y
        // no se separan nunca; con un número fijo, el del router se muere de
        // hambre porque no tiene el DRC que salva al del emulador.
        const double fps = runner.fps() > 1.0 ? runner.fps() : 60.0;
        size_t n = synth_frames_hint;
        if (n == 0) n = (size_t)(44100.0 / fps + 0.5);

        // Rendir MÁS frames de los que el device va a consumir es atraso que no
        // se recupera nunca (el resampler lo guarda y en régimen entra tanto
        // como sale). Lo que sobra se ceba: su estado entra, su audio no.
        // Un frame de holgura, no cero: apretado al límite exacto el resampler
        // se quedaba corto en el 1% de los bloques (necesita media ventana de
        // lookahead) y esos bloques salían con la cola en silencio — micro-cortes.
        // Un frame de más son ~17 ms de latencia, imperceptibles.
        const size_t budget = (size_t)((double)n * fps / 44100.0 + 2.5);
        while (voice_pending.size() > budget) {
            voice_router.mirror().prime_frame(voice_pending.front().writes.data(),
                                              (uint32_t)voice_pending.front().writes.size());
            voice_pending.erase(voice_pending.begin());
            ++vr_skipped;
        }

        voice_chip.clear();
        vr_pend_max = std::max<size_t>(vr_pend_max, voice_pending.size());
        std::vector<float> blk;
        for (auto& pf : voice_pending) {
            voice_router.tick(pf.writes.data(), (uint32_t)pf.writes.size(), pf.frame, blk);
            voice_chip.insert(voice_chip.end(), blk.begin(), blk.end());
            ++vr_frames;
        }
        voice_pending.clear();
        voice_rs.push(voice_chip.data(), voice_chip.size() / 2);

        // Red de seguridad: si aun así se juntó atraso, tirarlo. Un corte se oye
        // una vez; dos segundos de desfase con la imagen se oyen siempre. No
        // debería dispararse con lo de arriba puesto — si vr_resyncs sube, hay
        // otra fuente de desbalance y se ve acá en vez de deducirse de oído.
        if (voice_rs.available() > n * 3) { voice_rs.reset(); ++vr_resyncs; }
        voice_out.assign(n * 2, 0.0f);
        const size_t got = voice_rs.pull(voice_out.data(), n);
        // Faltó entrada: se completa con silencio y se cuenta. Si esto sube, el
        // router está entregando de menos y hay que mirar el pacing, no el
        // sonido — es el detector que a  le faltó durante diez causas raíz.
        if (got < n) ++vr_starved;
        // La voz del SoundFont entra acá (synth_tick corrió recién y dejó su
        // bloque listo): un solo camino de salida, sin dos productores peleando
        // por el stream.
        if (synth_pcm_ready && synth_pcm.size() >= n * 2) {
            double acc = 0.0;
            for (size_t i = 0; i < n * 2; ++i) {
                voice_out[i] += synth_pcm[i];
                acc += double(synth_pcm[i]) * synth_pcm[i];
            }
            // Nivel del bloque del SoundFont, para poder distinguir «no suena»
            // de «suena bajo» de «suena y se corta» sin depender del oído.
            const double r = n ? std::sqrt(acc / double(n * 2)) : 0.0;
            if (r > vr_sf2_peak) vr_sf2_peak = r;
            vr_sf2_rms = r;
            if (sf2_dump) {
                std::fwrite(synth_pcm.data(), sizeof(float), n * 2, sf2_dump);
                std::fflush(sf2_dump);
            }
        } else if (sf2_dump) {
            // Sin bloque del SF2 va SILENCIO, para que el archivo conserve el
            // eje de tiempo: si no, los huecos se cerrarían y no se vería que
            // el timbre dejó de sonar.
            static std::vector<float> z;
            z.assign(n * 2, 0.0f);
            std::fwrite(z.data(), sizeof(float), n * 2, sf2_dump);
        }
        vr_backlog = voice_rs.available();
        vr_queued  = audio.synth_queued_frames();
        if (voice_dump) {
            std::fwrite(voice_out.data(), sizeof(float), n * 2, voice_dump);
            std::fflush(voice_dump);
        }
        // El router OCUPA EL LUGAR del PCM del emulador: `buffer_router` pisa lo
        // staged. Así hereda todo el pacing de  —DRC y re-cebado tras un
        // stall— en vez de correr en un stream propio que no tiene nada: medido,
        // con stream propio la cola quedaba en 0 en casi todos los ticks, o sea
        // un corte por cada tirón del Lab.
        //
        // Y ESTE reemplazo es TODO lo que calla al chip (). Antes se lo
        // muteaba además a nivel core (0x3FF), y eso tenía un daño invisible:
        // el hasher de audio se alimenta del PCM del core, así que veía silencio
        // y las tomas grabadas con el router puesto salían SIN hashes de audio —
        // sin sustitución por lote y sin historia de audio. Se cazó al prenderlo
        // por defecto (audio_preview_smoke: «0 hashes de audio»).
        //
        // : en Sega CD no ocupa el lugar, se SUMA — ahí lo staged lleva el
        // chip PCM y el CDDA, que este router no espeja, y reemplazarlo dejaba
        // el sistema mudo. Lo que el router sí rinde ya viene callado del core.
        audio.buffer_router(voice_out.data(), n, router_mix());

        // Cada ~5 s. No por frame: un fprintf por frame ya se llevó el 92% del
        // «costo de upload» una vez (), y acá el dato no cambia tan rápido.
        if (vr_ticks % 300 == 0)
            std::fprintf(stdout,
                "[voice] ticks=%llu frames_chip=%llu cebados=%llu sin_entrada=%llu "
                "sustituidas=%llu atraso=%zu cola_max=%zu cebados_sueltos=%llu "
                "resyncs=%llu sf2_rms=%.5f sf2_pico=%.5f\n",
                (unsigned long long)vr_ticks, (unsigned long long)vr_frames,
                (unsigned long long)vr_primes, (unsigned long long)vr_starved,
                (unsigned long long)voice_router.stats().substituted,
                vr_backlog, vr_pend_max,
                (unsigned long long)vr_skipped, (unsigned long long)vr_resyncs,
                vr_sf2_rms, vr_sf2_peak);
        return true;
    }

    /// Escrituras de chip de TODA la toma, cacheadas por analyze_audio_events
    /// (que ya hace una pasada completa re-emulando). Es lo único con lo que se
    /// puede reconstruir el estado del espejo tras un seek: la grabación guarda
    /// inputs, no escrituras, así que sin este caché habría que re-emular.
    ///
    /// El tope existe porque un juego cargado de DAC escribe ~660 por frame:
    /// una toma larga llegaría a cientos de MB. Pasado el tope se descarta todo
    /// y el cebado degrada a reset — se oye un timbre equivocado un momento tras
    /// un seek, que es mucho mejor que quedarse sin memoria.
    static constexpr size_t kVoicePrimeCap = 4u * 1000u * 1000u;   // ~32 MB
    std::vector<AytherAudioWrite> voice_prime_writes;
    std::vector<uint32_t>         voice_prime_offsets;   // frame → índice de inicio
    bool                          voice_prime_capped = false;
    /// DUEÑO del caché: la toma cuyas escrituras están cacheadas. Sin esto,
    /// dos bugs: (a) una toma con audio_events.toml persistido nunca
    /// re-analiza → caché vacío → el espejo arranca EN FRÍO tras un seek (sin
    /// los patches FM del inicio de la canción, que no se reescriben nunca,
    /// el título quedaba fino/agudo para SIEMPRE — reporte 2026-08-19); y
    /// (b) cambiar de toma dejaba el caché de la anterior y el cebado primaba
    /// con las escrituras EQUIVOCADAS. replay_seek lo construye perezoso.
    const void*                   voice_prime_rec = nullptr;

    /// /: el caché se construye INCREMENTAL y acotado a `upto`, no
    /// de una pasada por toda la toma — con «Ax Game Play» (34.892 frames)
    /// la pasada completa eran 32 s de cuelgue sincrónico en el primer seek
    /// (el «playhead clavado» de ). Invariante: `voice_prime_offsets`
    /// tiene built+1 entradas = las escrituras de los frames [0, built).
    /// Quien extiende: (a) el playback secuencial, frame a frame y gratis
    /// (voice_prime_append desde voice_capture), (b) un seek más allá de lo
    /// construido, re-emulando bare SOLO el tramo [start, upto) con el mejor
    /// arranque que le pasa replay_seek (keyframe runtime/horneado o el
    /// savestate propio del final de lo construido).
    std::vector<uint8_t> voice_prime_state;        ///< máquina al final de lo construido
    uint32_t             voice_prime_state_frame = UINT32_MAX;   ///< frame de ese estado (built en su momento)

    /// Extiende el caché hasta `upto` sin ayuda del caller (para los
    /// exports): arranque = savestate propio si coincide con built, si no
    /// el estado inicial de la toma (re-corre bare lo ya cacheado).
    void voice_prime_ensure(uint32_t upto) {
        if (!replay_rec || voice_prime_rec != static_cast<const void*>(replay_rec)) return;
        const AytherRecording& rec = *replay_rec;
        const uint32_t built = voice_prime_built();
        if (voice_prime_capped || built >= (std::min)(upto, rec.frame_count())) return;
        if (!voice_prime_state.empty() && voice_prime_state_frame == built) {
            const std::vector<uint8_t> st = voice_prime_state;
            voice_prime_build(rec, upto, built, &st);
        } else {
            voice_prime_build(rec, upto, 0, &rec.initial_state);
        }
        replay_pos = -1;   // la máquina quedó en otro frame: el próximo seek re-posiciona
    }
    uint32_t voice_prime_built() const {
        return voice_prime_offsets.empty() ? 0u
             : static_cast<uint32_t>(voice_prime_offsets.size() - 1);
    }
    void voice_prime_reset(const AytherRecording& rec) {
        voice_prime_rec = &rec;
        voice_prime_writes.clear();
        voice_prime_offsets.assign(1, 0u);
        voice_prime_capped = false;
        voice_prime_state.clear();
        voice_prime_state_frame = UINT32_MAX;
    }
    /// Agrega las escrituras del frame `frame` si es EXACTAMENTE el siguiente
    /// a lo construido (contiguo). Devuelve true si las tomó.
    bool voice_prime_push(uint32_t frame, const AytherAudioWrite* w, uint32_t wc) {
        if (voice_prime_capped || voice_prime_offsets.empty()) return false;
        if (frame != voice_prime_built()) return false;
        if (voice_prime_rec && frame >= static_cast<const AytherRecording*>(voice_prime_rec)->frame_count())
            return false;
        if (wc) {
            if (voice_prime_writes.size() + wc > kVoicePrimeCap) {
                voice_prime_capped = true;
                voice_prime_writes.clear();
                voice_prime_writes.shrink_to_fit();
                voice_prime_offsets.clear();
                voice_prime_state.clear();
                std::fprintf(stdout,
                    "[voice] toma demasiado larga para cachear escrituras: "
                    "el cebado tras un seek degrada a reset\n");
                return false;
            }
            voice_prime_writes.insert(voice_prime_writes.end(), w, w + wc);
        }
        voice_prime_offsets.push_back(static_cast<uint32_t>(voice_prime_writes.size()));
        return true;
    }
    /// Extiende el caché hasta `upto` (exclusivo) re-emulando bare desde
    /// `start` con `start_state` (start ≤ built: los frames [start, built)
    /// se corren sin capturar). Deja la máquina al final del tramo: el
    /// caller la reposiciona (replay_seek lo hace siempre). Guarda un
    /// savestate al final para que la próxima extensión no vuelva a 0.
    void voice_prime_build(const AytherRecording& rec, uint32_t upto,
                           uint32_t start, const std::vector<uint8_t>* start_state) {
        if (voice_prime_rec != &rec) voice_prime_reset(rec);
        const uint32_t n = rec.frame_count();
        if (upto > n) upto = n;
        if (voice_prime_capped) return;
        const uint32_t built = voice_prime_built();
        if (built >= upto) return;
        if (!start_state || start_state->empty() || start > built ||
            !runner.unserialize(*start_state)) {
            voice_prime_offsets.clear();   // sin arranque válido: cache inválido → fallback a reset
            return;
        }
        for (uint32_t f = start; f < upto; ++f) {
            runner.set_input(0, rec.inputs[f]);
            runner.run_frame();
            if (f < built) continue;          // ya cacheado: solo avanzar la máquina
            const AytherAudioWrite* w = nullptr;
            uint32_t wc = 0;
            ayther_frame_snapshot_v1 bs{};
            if (runner.capture_frame_snapshot(bs).ok()) {
                abi_audio.resize(bs.audio_write_count);
                const auto rb = runner.read_audio_writes_v1(
                    abi_audio.data(), static_cast<uint32_t>(abi_audio.size()), bs);
                if (rb.ok()) {
                    w  = reinterpret_cast<const AytherAudioWrite*>(abi_audio.data());
                    wc = rb.count;
                }
            } else {
                AYTHER_LEGACY_READ_BEGIN
                w  = reinterpret_cast<const AytherAudioWrite*>(runner.audio_writes());
                wc = runner.audio_write_count();
                AYTHER_LEGACY_READ_END
            }
            if (!voice_prime_push(f, w, wc)) return;   // capped
        }
        if (upto < n) {
            std::vector<uint8_t> st;
            if (runner.serialize(st) && !st.empty()) {
                voice_prime_state       = std::move(st);
                voice_prime_state_frame = upto;
            }
        } else {
            voice_prime_state.clear();
            voice_prime_state_frame = UINT32_MAX;
        }
    }

    /// Reconstruye el estado de registros del espejo hasta `target` SIN generar
    /// audio. Barato: son escrituras a registros, no síntesis.
    /// : cuantos frames del cebado se RINDEN de verdad (el resto solo
    /// repone registros). Dos segundos a 60 fps — ver voice_prime_to().
    static constexpr uint32_t kVoicePrimeRenderFrames = 120;

    void voice_prime_to(uint32_t target) {
        voice_router.reset();
        voice_rs.reset();
        voice_pending.clear();
        ++vr_primes;
        if (voice_prime_offsets.size() < 2) return;
        const uint32_t last = std::min<uint32_t>(
            target, uint32_t(voice_prime_offsets.size() - 1));
        // : los ULTIMOS frames del cebado se RINDEN (audio generado y
        // descartado) en vez de solo reponer registros.
        //
        // POR QUE. `prime_frame` escribe los registros al chip y adelanta el
        // reloj SIN generar muestras (`generated_ = frame_base_/...`), asi que
        // las ENVOLVENTES nunca evolucionan: una nota que llevaba tres segundos
        // sonando llegaba al frame destino con su envolvente en el arranque, y
        // el espejo entregaba una version DELGADA de la musica. Medido con
        // `tools/audio_seek_probe` sobre «Musica intro» f0=400: -74,3 % de
        // nivel con el router puesto, y 0,0 % con el router apagado — el chip
        // del emulador nunca fue el problema.
        //
        // Rendir TODO el tramo seria correcto y carisimo (un seek al minuto 5
        // son 18.000 frames de sintesis). Con dos segundos alcanza: es varias
        // veces el attack+decay mas largo del YM2612, asi que la envolvente
        // llega al mismo sustain que llegaria emulando desde el principio.
        const uint32_t warm = last < kVoicePrimeRenderFrames
                                  ? last : kVoicePrimeRenderFrames;
        const uint32_t cut  = last - warm;
        for (uint32_t i = 0; i < last; ++i) {
            const uint32_t a = voice_prime_offsets[i];
            const uint32_t b = voice_prime_offsets[i + 1];
            if (b <= a) continue;
            if (i < cut)
                voice_router.mirror().prime_frame(voice_prime_writes.data() + a, b - a);
            else
                voice_router.mirror().run_frame(voice_prime_writes.data() + a, b - a);
        }
    }

    AytherSf2* synth_for(uint64_t inst) {
        auto it = synths.find(inst);
        return it == synths.end() ? nullptr : it->second;
    }

    /// Carga un SoundFont (pack primero, disco después — mismo criterio que
    /// sprites y poses: el artista asigna archivos sueltos y ESCUCHA antes de
    /// hornear) y devuelve una instancia NUEVA de sintetizador. El parse se
    /// comparte por clave (ayther_sf2_new_shared), así que N instancias del
    /// mismo archivo no lo duplican. nullptr si no se pudo — el caller decide
    /// si loguea.
    AytherSf2* load_sf2_shared(const std::string& sf) {
        if (sf.empty()) return nullptr;
        const uint64_t sfkey = std::hash<std::string>{}(sf);
        if (pack) {
            const int64_t sz = ayther_pack_file_size(pack.get(), sf.c_str());
            if (sz > 0) {
                std::vector<uint8_t> buf(static_cast<size_t>(sz));
                if (ayther_pack_read(pack.get(), sf.c_str(), buf.data(), buf.size()) > 0)
                    if (AytherSf2* sy = ayther_sf2_new_shared(sfkey, buf.data(),
                                                              buf.size(), 44100))
                        return sy;
            }
        }
        // Un .sfz se normaliza por RUTA (sus samples viven al lado del texto);
        // .sf2/.sf3 van crudos — la conversión SF3 la hace ayther_sf2_new_shared
        // por detección de bytes, una sola vez por key (queda en su cache).
        const bool sfz = sf.size() > 4 &&
            [&] {
                std::string ext = sf.substr(sf.size() - 4);
                for (auto& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return ext == ".sfz";
            }();
        if (sfz) {
            const size_t need =
                ayther_soundfont_normalize_file(sf.c_str(), nullptr, 0);
            if (need == 0) return nullptr;
            std::vector<uint8_t> norm(need);
            if (ayther_soundfont_normalize_file(sf.c_str(), norm.data(),
                                                norm.size()) != need)
                return nullptr;
            return ayther_sf2_new_shared(sfkey, norm.data(), norm.size(), 44100);
        }
        std::FILE* fh = std::fopen(sf.c_str(), "rb");
        if (!fh) return nullptr;
        std::fseek(fh, 0, SEEK_END);
        const long fsz = std::ftell(fh);
        std::fseek(fh, 0, SEEK_SET);
        AytherSf2* sy = nullptr;
        if (fsz > 0) {
            std::vector<uint8_t> fbuf(static_cast<size_t>(fsz));
            if (std::fread(fbuf.data(), 1, fbuf.size(), fh) == fbuf.size())
                sy = ayther_sf2_new_shared(sfkey, fbuf.data(), fbuf.size(), 44100);
        }
        std::fclose(fh);
        return sy;
    }

    /// : la base del export por el ROUTER, offline. Las MISMAS decisiones
    /// que la preview (SessionPolicy para las voces del chip + las reglas de
    /// synth_tick para el SoundFont) aplicadas sobre el cache de escrituras de
    /// la toma — sin device, sin pacing, sin DRC. La preview y el entregable
    /// dejan de ser dos renders distintos, y el residuo que el detector no
    /// puede modelar (: ring post key-off, pokes sin key) no llega al WAV:
    /// una voz silenciada no EMITE, no hay ventana que acertar.
    ///
    /// Instancias PROPIAS de router y sintetizadores: el vivo puede estar
    /// sonando mientras se exporta y no se le puede tocar ni el estado del
    /// espejo ni las voces en vuelo.
    ///
    /// Devuelve false si no puede (router apagado, toma sin analizar, cache de
    /// escrituras capped) — el caller cae al camino viejo (máscara sustractiva
    /// sobre el emulador).
    bool export_router_base(uint32_t start, uint32_t win, double fps,
                            size_t max_frames, std::vector<int16_t>& out) {
        out.clear();
        if (!voice_router_on || audio_events.empty()) return false;
        // : el caché es incremental — un export más allá de lo
        // construido lo extiende (offline: desde el savestate propio si está
        // al final de lo construido, si no desde el inicio de la toma).
        voice_prime_ensure(start + win);
        if (voice_prime_capped || voice_prime_offsets.size() < 2) return false;
        const uint32_t cached = static_cast<uint32_t>(voice_prime_offsets.size() - 1);
        if (start >= cached) return false;
        const uint32_t last = (std::min)(start + win, cached);

        ChannelRouter router;
        SessionPolicy pol;
        pol.im = this;
        router.set_policy(&pol);
        const double sfps = runner.fps();
        router.mirror().set_pal(sfps > 1.0 && sfps < 55.0);
        for (uint32_t i = 0; i < start; ++i) {
            const uint32_t a = voice_prime_offsets[i], b = voice_prime_offsets[i + 1];
            if (b > a) router.mirror().prime_frame(voice_prime_writes.data() + a, b - a);
        }
        StreamResampler rs;
        rs.set_rates(router.mirror().rate(), 44100.0);

        // SoundFont offline: sintetizadores FRESCOS por timbre y notas en
        // vuelo con las reglas de synth_tick — sin su catch-up, porque acá
        // cada frame es un frame. Mismo gate que el vivo (synth_tick:861).
        const bool with_synth = synth_any && audio_sub_preview;
        std::unordered_map<uint64_t, AytherSf2*> osy;
        auto osy_for = [&](uint64_t inst) -> AytherSf2* {
            auto it = osy.find(inst);
            if (it != osy.end()) return it->second;
            AytherSf2* sy = nullptr;
            const auto ia = inst_assign.find(inst);
            if (ia != inst_assign.end()) {
                sy = load_sf2_shared(ia->second.soundfont);
                if (sy) ayther_sf2_program(sy, 0, ia->second.preset);
            }
            osy.emplace(inst, sy);   // se cachea aunque sea nulo (no reintentar)
            return sy;
        };
        struct ONote { uint64_t inst, occ; int key; int64_t ends; bool one_shot; };
        std::vector<ONote> on;

        std::vector<float> blk, mixf, tmp;
        // Saca del resampler lo disponible, suma el bloque del SoundFont
        // (mismo largo — un solo camino de salida, como voice_tick) y convierte
        // EXACTAMENTE como buffer_router: lrint ×32767 con clamp.
        auto flush_avail = [&]() {
            const size_t avail = rs.available();
            if (!avail || out.size() / 2 >= max_frames) return;
            mixf.assign(avail * 2, 0.0f);
            rs.pull(mixf.data(), avail);
            if (with_synth) {
                for (auto& [inst, sy] : osy) {
                    if (!sy) continue;
                    tmp.assign(avail * 2, 0.0f);
                    ayther_sf2_render(sy, tmp.data(), avail);
                    const auto ia = inst_assign.find(inst);
                    const float g = (ia != inst_assign.end() && ia->second.gain > 1.0f)
                                        ? ia->second.gain : 1.0f;
                    for (size_t i = 0; i < tmp.size(); ++i) mixf[i] += tmp[i] * g;
                }
            }
            const size_t room = max_frames * 2 - out.size();
            const size_t take = (std::min)(mixf.size(), room);
            for (size_t i = 0; i < take; ++i) {
                const int v = static_cast<int>(std::lrint(mixf[i] * 32767.0f));
                out.push_back(static_cast<int16_t>(
                    v > 32767 ? 32767 : (v < -32768 ? -32768 : v)));
            }
        };

        for (uint32_t f = start; f < last && out.size() / 2 < max_frames; ++f) {
            const uint32_t a = voice_prime_offsets[f], b = voice_prime_offsets[f + 1];
            router.tick(voice_prime_writes.data() + a, b - a, f, blk);
            rs.push(blk.data(), blk.size() / 2);

            if (with_synth) {
                // Cierres primero (dos notas contiguas comparten frontera), y
                // por nota, no por instrumento — ver la saga en synth_tick.
                for (size_t i = on.size(); i-- > 0;) {
                    const ONote& sn = on[i];
                    const bool muted = occurrence_muted(sn.occ, sn.inst);
                    if (sn.one_shot && !muted) continue;
                    if (!muted && static_cast<int64_t>(f) <= sn.ends) continue;
                    if (AytherSf2* sy = osy_for(sn.inst))
                        ayther_sf2_note_off(sy, 0, sn.key);
                    on.erase(on.begin() + static_cast<long>(i));
                }
                for (const AytherAudioEvent& e : audio_events) {
                    if (e.start_frame != f) continue;
                    if (e.pitch == 255) continue;          // DAC/ruido: sin nota
                    const auto ia = inst_assign.find(e.instrument);
                    if (ia == inst_assign.end()) continue;
                    if (event_muted(e)) continue;          // silenciado entero ()
                    // Prioridad ASSET > SF2 — mismo predicado que synth_tick:
                    // el mixdown debe sonar como el vivo.
                    if (event_covered_by_asset(e, f)) continue;
                    AytherSf2* sy = osy_for(e.instrument);
                    if (!sy) continue;
                    const int key = static_cast<int>(e.pitch) + ia->second.transpose;
                    if (key < 0 || key > 127) continue;
                    const int vel = e.velocity ? static_cast<int>(e.velocity) : 100;
                    for (size_t i = on.size(); i-- > 0;)
                        if (on[i].inst == e.instrument && on[i].key == key) {
                            ayther_sf2_note_off(sy, 0, key);
                            on.erase(on.begin() + static_cast<long>(i));
                        }
                    ayther_sf2_control(sy, 0, 7,
                        std::clamp(static_cast<int>(
                            (std::min)(ia->second.gain, 1.0f) * 127.0f + 0.5f), 0, 127));
                    ayther_sf2_note_on(sy, 0, key, vel);
                    on.push_back({ e.instrument,
                                   occ_key(e.chip, e.channel, e.start_frame),
                                   key, static_cast<int64_t>(e.end_frame),
                                   synth_one_shot(e.instrument) });
                }
            }
            flush_avail();
        }
        // Colita del resampler (media ventana de lookahead): offline no hay
        // próximo tick que la drene, se empuja un frame de silencio.
        blk.assign(static_cast<size_t>(router.mirror().rate() / fps + 1.0) * 2, 0.0f);
        rs.push(blk.data(), blk.size() / 2);
        flush_avail();

        for (auto& [_, sy] : osy)
            if (sy) ayther_sf2_free(sy);
        ayther_sf2_trim_cache();
        return out.size() >= 2;
    }

    /// : render offline AISLADO desde el espejo — el sustrato de los
    /// previews A/B y del WAV de eventos. A diferencia de export_router_base
    /// (que COMPONE lo que se entrega), acá se AÍSLA lo que se audita:
    ///
    ///   solo_mask   — canales enteros (el «Original, canal aislado» del A/B)
    ///   member_sigs — sólo las voces de esas firmas, con el modelo de 
    ///                 del lado de la FUENTE: ventana propia + cola de release
    ///                 que cede ante un evento ajeno del canal, y la exclusión
    ///                 del DAC ajeno en FM6 (S26 en «Melodía», 2026-07-23).
    ///
    /// El espejo da los 10 canales POR SEPARADO, así que aislar es sumar los
    /// que pasan el gate — lo que queda afuera es CERO digital, no un mute que
    /// deja pasar el residuo de . Sin SF2 ni sustituciones: el aislamiento
    /// se usa para escuchar el ORIGINAL de un canal o de un sonido.
    ///
    /// Devuelve false (→ el caller cae al camino viejo de captura) si no hay
    /// aislamiento pedido, el router está apagado o la toma no tiene cache.
    bool preview_render_base(uint32_t start, uint32_t win,
                             size_t max_frames, uint32_t solo_mask,
                             const std::vector<uint64_t>* member_sigs,
                             std::vector<int16_t>& out) {
        out.clear();
        const bool by_events = member_sigs && !member_sigs->empty() &&
                               !audio_events.empty();
        if (!by_events && !solo_mask) return false;   // mezcla completa: camino viejo
        if (!voice_router_on) return false;           // coherencia con lo que se oye
        // : el caché es incremental — un export más allá de lo
        // construido lo extiende (offline: desde el savestate propio si está
        // al final de lo construido, si no desde el inicio de la toma).
        voice_prime_ensure(start + win);
        if (voice_prime_capped || voice_prime_offsets.size() < 2) return false;
        const uint32_t cached = static_cast<uint32_t>(voice_prime_offsets.size() - 1);
        if (start >= cached) return false;
        const uint32_t last = (std::min)(start + win, cached);

        ChipMirror mirror;
        const double sfps = runner.fps();
        mirror.set_pal(sfps > 1.0 && sfps < 55.0);
        for (uint32_t i = 0; i < start; ++i) {
            const uint32_t a = voice_prime_offsets[i], b = voice_prime_offsets[i + 1];
            if (b > a) mirror.prime_frame(voice_prime_writes.data() + a, b - a);
        }
        StreamResampler rs;
        rs.set_rates(mirror.rate(), 44100.0);

        std::vector<float> blk, mixf;
        auto flush_avail = [&]() {
            const size_t avail = rs.available();
            if (!avail || out.size() / 2 >= max_frames) return;
            mixf.assign(avail * 2, 0.0f);
            rs.pull(mixf.data(), avail);
            const size_t room = max_frames * 2 - out.size();
            const size_t take = (std::min)(mixf.size(), room);
            for (size_t i = 0; i < take; ++i) {
                const int v = static_cast<int>(std::lrint(mixf[i] * 32767.0f));
                out.push_back(static_cast<int16_t>(
                    v > 32767 ? 32767 : (v < -32768 ? -32768 : v)));
            }
        };

        constexpr uint32_t kTail = 15;   // = kMuteTailFrames ()
        for (uint32_t f = start; f < last && out.size() / 2 < max_frames; ++f) {
            const uint32_t a = voice_prime_offsets[f], b = voice_prime_offsets[f + 1];
            mirror.run_frame(voice_prime_writes.data() + a, b - a);
            const size_t ns = mirror.frame_samples();

            uint32_t gate = solo_mask;
            if (by_events) {
                uint16_t own = 0, tail = 0, foreign = 0, fdac = 0;
                for (const AytherAudioEvent& e : audio_events) {
                    const uint32_t bit = chan_bit(e.chip, e.channel);
                    const bool member = std::find(member_sigs->begin(),
                                                  member_sigs->end(),
                                                  e.signature) != member_sigs->end();
                    if (member) {
                        if (f >= e.start_frame && f <= e.end_frame)          own  |= bit;
                        else if (f > e.end_frame && f <= e.end_frame + kTail) tail |= bit;
                    } else if (f >= e.start_frame && f <= e.end_frame) {
                        foreign |= bit;
                        // DAC ajeno: comparte la salida física de FM6 y PISA la
                        // nota — mientras suene, el canal queda afuera.
                        if (e.chip == 0 && e.channel == 5 && e.pitch > 127)
                            fdac |= bit;
                    }
                }
                gate = static_cast<uint16_t>((own | (tail & ~foreign)) & ~fdac);
            }

            blk.assign(ns * 2, 0.0f);
            if (gate) {
                for (int c = 0; c < ChipMirror::kChannels; ++c) {
                    if (!(gate & (1u << c))) continue;
                    const float* ch = mirror.channel(c);
                    for (size_t i = 0; i < ns * 2; ++i) blk[i] += ch[i];
                }
            }
            rs.push(blk.data(), ns);
            flush_avail();
        }
        // Colita del resampler: medio frame de lookahead — offline no hay
        // próximo tick que la drene.
        const double fps_drain = sfps > 1.0 ? sfps : 60.0;
        blk.assign(static_cast<size_t>(mirror.rate() / fps_drain + 1.0) * 2, 0.0f);
        rs.push(blk.data(), blk.size() / 2);
        flush_avail();
        // NO-VACUIDAD. El espejo puede devolver muestras y que sean TODAS CERO,
        // y hasta acá eso contaba como éxito: el caller se quedaba con el
        // silencio en vez de caer al core.
        //
        // Pasa cuando el span arranca al PRINCIPIO de la toma: el priming de
        // arriba (`for i < start`) no tiene frames que recorrer, así que el
        // mirror no hereda la configuración de instrumentos que el juego había
        // escrito ANTES de la grabación — la toma arranca de un savestate y
        // esas escrituras no están en ella. Los key-on del span suenan en vacío.
        //
        // Reporte 2026-08-06: la Secuencia «Opening» (frames 0-35 de Demo
        // Amazona) no sonaba ni en el preview ni en la reproducción; medido con
        // tools/seq_preview_probe, el espejo daba rms 0 y el CORE rms 1983 para
        // el mismo span. Devolver «no pude» hace caer al camino del core, que
        // sí tiene el audio. Esto no INVENTA sonido: un span mudo de verdad
        // sigue mudo por el core; lo que deja de haber es un éxito vacío.
        for (int16_t v : out)
            if (v != 0) return out.size() >= 2;
        out.clear();
        return false;
    }

    // -- LA PREGUNTA ÚNICA DEL MUTE () -----------------------------------
    //
    // Silenciar es silenciar. Hasta acá el mute y la SUSTITUCIÓN eran dos
    // caminos separados y sólo uno miraba el mute: la voz del chip callaba y el
    // asset HD (o el SoundFont) que la reemplaza seguía sonando — o sea que el
    // altavoz apagaba el original y dejaba el reemplazo, justo al revés de lo
    // que el artista pide. La causa de fondo era tener la decisión escrita tres
    // veces: en `voice_replaced` (chip), en los disparos de HD (que no la
    // tenían) y en `synth_tick` (sólo por instrumento).
    //
    // Estas tres funciones son ESA decisión, en un solo lugar, y las consumen
    // los cuatro caminos: el router, los disparos de HD, el sintetizador y el
    // mixdown del export.
    /// El bit de canal vive en el namespace del archivo (ver `chan_bit` arriba):
    /// lo usan tanto los métodos de Impl como las funciones libres de más abajo.
    /// La clave de ocurrencia del Lab (lab::audio_event_key).
    static uint64_t occ_key(uint8_t chip, uint8_t channel, uint32_t start) {
        return (static_cast<uint64_t>(chip) << 56) |
               (static_cast<uint64_t>(channel) << 48) | start;
    }

    /// ¿Hay ALGÚN mute puesto? Atajo para no pagar un escaneo en el caso
    /// común, que es el de siempre: nada silenciado.
    bool any_mute() const {
        return !audio_instrument_mute.empty() || !audio_occurrence_mute.empty() ||
               audio_manual_mute != 0;
    }

    /// ¿Está silenciada esta ocurrencia? Los tres ejes del panel Mezclar:
    /// el altavoz de un Sonido o de un Patrón (por instrumento), el ojo de una
    /// Secuencia (por ocurrencia) y el mute a mano del timeline (por canal).
    bool occurrence_muted(uint64_t occ, uint64_t inst) const {
        if (audio_instrument_mute.count(inst)) return true;
        if (!audio_occurrence_mute.empty() && audio_occurrence_mute.count(occ))
            return true;
        return (audio_manual_mute &
                chan_bit(static_cast<uint8_t>(occ >> 56),
                         static_cast<uint8_t>(occ >> 48))) != 0;
    }

    bool event_muted(const AytherAudioEvent& e) const {
        // : el bus del sonido calla la voz ORIGINAL igual que el altavoz
        // del artista. Es lo que hace que silenciar el bus de Música se lleve
        // también la música del juego, y no sólo su reemplazo HD.
        if (bus_muted_for(e.signature)) return true;
        if (!any_mute()) return false;
        return occurrence_muted(occ_key(e.chip, e.channel, e.start_frame),
                                e.instrument);
    }

    /// ¿Está silenciada esta Secuencia en la ventana anclada en `anchor`?
    ///
    /// El HD de una Secuencia es UNA mezcla de varias voces: no se puede
    /// silenciar a medias. Calla cuando NINGUNA de sus voces queda audible —
    /// que es exactamente lo que hacen los dos gestos del artista: el altavoz
    /// de un Patrón silencia todos sus timbres, y el ojo de la Secuencia
    /// silencia todas sus ocurrencias. Silenciar UN timbre de una Secuencia de
    /// varios no la calla, y eso es correcto: el asset sigue teniendo voces
    /// vivas que representar.
    bool seq_sub_muted(const AudioSeqSub& sq, uint32_t anchor) const {
        if (!any_mute()) return false;
        const uint32_t end =
            anchor + (sq.duration_frames ? sq.duration_frames : 1u);
        bool any_member = false;
        for (const auto& e : audio_events) {
            if (e.start_frame >= end) break;        // audio_events va en orden
            if (e.start_frame < anchor) continue;
            if (e.signature != sq.trigger_signature &&
                std::find(sq.signatures.begin(), sq.signatures.end(),
                          e.signature) == sq.signatures.end())
                continue;
            any_member = true;
            if (!event_muted(e)) return false;      // queda algo audible
        }
        return any_member;
    }

    /// Variante por FIRMA, para el camino EN VIVO (Capturar) y el del pack:
    /// ahí no hay toma analizada con la ocurrencia exacta, sólo la firma del
    /// canal que acaba de arrancar. Se resuelve contra los eventos analizados
    /// si los hay; sin ellos, sólo puede aplicar el mute por canal. `bit` es el
    /// canal donde suena (0 = no se sabe).  F3: si la voz trae su timbre
    /// (`instr`, del FFI de actives), el mute por instrumento se decide directo
    /// — una firma variante no está en los eventos analizados y antes escapaba.
    bool signature_muted(uint64_t sig, uint32_t bit, uint64_t instr = 0) const {
        // : el bus va ANTES del cortocircuito de `any_mute()`, que sólo
        // mira los mutes del ARTISTA (altavoz, ojo, canal). Un bus silenciado
        // es otra cosa —una decisión de mezcla, no de autoría— y si se
        // preguntara después, silenciar la música no haría nada en un proyecto
        // donde nadie tocó un altavoz.
        if (bus_muted_for(sig)) return true;
        if (!any_mute()) return false;
        if (bit && (audio_manual_mute & bit)) return true;
        if (audio_instrument_mute.empty()) return false;
        if (instr) return audio_instrument_mute.count(instr) != 0;
        for (const auto& e : audio_events)
            if (e.signature == sig)
                return audio_instrument_mute.count(e.instrument) != 0;
        return false;
    }

    /// La Secuencia EN VIVO. Sin toma analizada no hay ventana que mirar, así
    /// que la pregunta se hace sobre sus firmas miembro y su máscara de
    /// canales — mismo criterio que seq_sub_muted: calla cuando no le queda
    /// ninguna voz audible.
    bool seq_sub_muted_live(const AudioSeqSub& sq) const {
        if (!any_mute()) return false;
        if (sq.channel_mask &&
            (audio_manual_mute & sq.channel_mask) == sq.channel_mask) return true;
        if (audio_instrument_mute.empty() || sq.signatures.empty()) return false;
        for (uint64_t sig : sq.signatures)
            if (!signature_muted(sig, 0)) return false;
        return true;
    }

    /// Disparos de HD que NO sonaron por estar silenciados, y streams que hubo
    /// que CORTAR porque el artista silenció a mitad del asset. Sin esto, «el
    /// mute no funciona» y «el mute funciona pero el asset largo sigue hasta
    /// que termina» se ven idénticos desde afuera — y son arreglos distintos.
    uint64_t hd_muted = 0, hd_cut = 0;
    uint64_t hd_claimed = 0;   ///< : disparos reclamados por otra Secuencia (vivo)

    /// Bits de mute por OCURRENCIA (Secuencias deshabilitadas) en el frame f.
    /// La ventana se EXTIENDE kOccMuteTailFrames tras el end: el release
    /// FM/PSG sigue sonando tras el key-off y las colas se oían como una
    /// melodía fantasma «degradada» (reporte 2026-07-23). Excepción: un
    /// evento AJENO (no muteado) activo en el canal debe oírse.
    /// Cola de release del FM. La ventana de un evento va del key-on al
    /// key-off, pero el chip NO calla en el key-off: ahí ARRANCA el release. Un
    /// mute que cierra en `end_frame` destapa esa cola, y lo que se oye al
    /// reaparecer es un salto de amplitud — el «clic suave» del reporte del
    /// 2026-07-27.
    ///
    /// El 15 no es una corazonada: medido con tools/mute_silence_probe sobre
    /// Demo Barbaro, el residuo suelto cae de -29,8 a -42,2 dBFS al llegar a
    /// ~10 frames y de ahí se ESTANCA (60 y 120 frames no mejoran nada). Esa
    /// meseta es la cola; lo que queda después son eventos que el detector no
    /// vio, y alargar el mute no los arregla — sólo se comería audio bueno.
    static constexpr uint32_t kMuteTailFrames = 15;   // ~250 ms

    /// Máscara por-evento con cola de release. `want(e)` decide qué eventos se
    /// silencian; los demás DEFIENDEN su canal durante la cola.
    ///
    /// La ventana propia [start,end] se mutea siempre, igual que antes; sólo la
    /// COLA cede ante un evento ajeno. Así el arreglo es estrictamente aditivo
    /// —nunca desmutea algo que hoy se mutea— y no puede reabrir el «se oyen
    /// los dos superpuestos» que costó una escucha arreglar. Sin esa asimetría,
    /// extender la cola se llevaría puesto el golpe de espada que cae dos
    /// frames después de una nota de la música en el mismo canal FM.
    template <class Fn>
    uint32_t event_mute_with_tail(uint32_t f, Fn&& want) const {
        uint32_t own = 0, tail = 0, foreign = 0;
        for (const AytherAudioEvent& e : audio_events) {
            const uint32_t bit = chan_bit(e.chip, e.channel);
            if (want(e)) {
                if (f >= e.start_frame && f <= e.end_frame)          own  |= bit;
                else if (f > e.end_frame &&
                         f <= e.end_frame + kMuteTailFrames)         tail |= bit;
            } else if (f >= e.start_frame && f <= e.end_frame) {
                foreign |= bit;
            }
        }
        return own | (tail & ~foreign);
    }

    uint32_t occurrence_mute_at(uint32_t f) const {
        if (audio_occurrence_mute.empty()) return 0;
        // Éste aplica `foreign` a TODA la ventana, no sólo a la cola (ver
        // event_mute_with_tail): acá el mute es «esta ocurrencia no suena» y
        // otra aparición del mismo sonido debe seguir sonando. Semántica
        // distinta a propósito — por eso no comparte el helper.
        constexpr uint32_t kOccMuteTailFrames = kMuteTailFrames;
        uint32_t muted = 0, foreign = 0;
        for (const auto& e : audio_events) {
            const uint32_t bit = chan_bit(e.chip, e.channel);
            const uint64_t k = (static_cast<uint64_t>(e.chip) << 56) |
                               (static_cast<uint64_t>(e.channel) << 48) |
                               e.start_frame;
            if (audio_occurrence_mute.count(k)) {
                if (f >= e.start_frame && f <= e.end_frame + kOccMuteTailFrames)
                    muted |= bit;
            } else if (f >= e.start_frame && f <= e.end_frame) {
                foreign |= bit;
            }
        }
        return static_cast<uint32_t>(muted & ~foreign);
    }

    /// Ancla de la ventana de una sub de Secuencia en el frame f. Las
    /// ocurrencias de la firma disparadora se SEGMENTAN greedy: una que cae
    /// DENTRO de la ventana anclada por una anterior es INTERNA y NO ancla —
    /// la melodía repite su primera nota y re-anclar «en la más reciente»
    /// reiniciaba el HD ~1 s después del arranque (reporte 2026-07-23). Una
    /// repetición REAL (tras el fin de la ventana) sí re-ancla y re-dispara.
    /// audio_events va en orden de frame (el detector procesa secuencial).
    bool seq_sub_anchor(const AudioSeqSub& sq, uint32_t f,
                        uint32_t* anchor) {
        const auto& anchors = seq_anchors_of(sq);
        bool found = false;
        for (const uint32_t a : anchors) {
            if (a > f) break;                           // orden ascendente
            if (f < a + sq.duration_frames) {
                found   = true;   // el ancla MÁS RECIENTE que cubre f gana
                *anchor = a;
            }
        }
        return found;
    }

    /// : anclas de TODAS las subs en UNA pasada conjunta (segmentación
    /// greedy + RECLAMO entre Secuencias + prioridad en el empate) — el
    /// algoritmo vive en audio_seq_anchor.h (puro, con su oráculo). Caso
    /// real (reporte 2026-08-21, Golden Axe): «The Battle - Intro» y «- Loop»
    /// comparten 26 firmas y sonaban las dos a la vez.
    /// : las subs de Secuencia como las ve el anclaje (replay y vivo).
    std::vector<SeqAnchorSub> seq_anchor_view() const {
        std::vector<SeqAnchorSub> subs;
        subs.reserve(audio_seq_subs.size());
        for (const auto& s : audio_seq_subs) {
            SeqAnchorSub v;
            v.key = s.key; v.trigger_signature = s.trigger_signature;
            v.duration_frames = s.duration_frames; v.span_frames = s.span_frames;
            v.enabled = !s.asset.empty(); v.signatures = s.signatures;
            v.head = s.head_signatures; v.looping = s.looping;
            subs.push_back(std::move(v));
        }
        return subs;
    }
    const std::vector<uint32_t>& seq_anchors_of(const AudioSeqSub& sq) {
        if (seq_anchor_for_n != audio_events.size() ||
            seq_anchor_for_gen != audio_seq_subs_gen) {
            seq_anchor_for_n   = audio_events.size();
            seq_anchor_for_gen = audio_seq_subs_gen;
            const std::vector<SeqAnchorSub> subs = seq_anchor_view();
            seq_anchor_cache = seq_anchor_table(
                audio_events.size(),
                [&](size_t i) { return audio_events[i].signature; },
                [&](size_t i) { return audio_events[i].start_frame; }, subs);
        }
        static const std::vector<uint32_t> kNone;
        const auto it = seq_anchor_cache.find(sq.key);
        return it == seq_anchor_cache.end() ? kNone : it->second;
    }

    /// Prioridad ASSET > SF2 > original (reporte 2026-07-31): ¿en el frame f
    /// este evento lo está tocando un ASSET? Entonces el SoundFont NO lo
    /// sintetiza — antes sonaban superpuestos (el asset de la Secuencia y el
    /// SF2 del timbre a la vez). Cubre los dos caminos de asset: la asignación
    /// directa por firma y la membresía en una Secuencia con asset cuya
    /// ventana (seq_sub_anchor) cubre f. La voz original ya calla por el mute
    /// dinámico / buffer_router — este predicado sólo resuelve asset vs SF2.
    /// Consultado por synth_tick (vivo) Y por el mixdown offline: misma
    /// decisión en un solo lugar, el criterio de .
    /// : «lo toca un asset» = el asset está LISTO (hd_can_sound) — con el
    /// asset roto el SF2 sí sintetiza (prioridad ASSET > SF2 > original: el
    /// escalón que falla se saltea, no deja silencio). No-const por readiness.
    bool event_covered_by_asset(const AytherAudioEvent& e, uint32_t f) {
        //  F3: exacta o por regla — la ocurrencia variante también la
        // toca el asset (el disparo del replay resuelve con el mismo criterio).
        uint64_t asig = 0;
        if (resolve_event_sig(e.signature, e.instrument, e.pitch, &asig))
            if (const auto it = audio_event_assign.find(asig);
                it != audio_event_assign.end() &&
                // : una firma BLOQUEADA por sus condiciones no está
                // cubierta. Sin esto el mute seguía aplicándose y el resultado
                // era SILENCIO: ni el HD (gateado) ni el original (muteado).
                // La decisión de mute es única (), así que el gate tiene
                // que entrar acá y no sólo en el disparo.
                !audio_gated(asig) &&
                hd_can_sound(asig, it->second)) return true;
        for (const auto& sq : audio_seq_subs) {
            if (sq.asset.empty() || !hd_can_sound(sq.key, sq.asset)) continue;
            if (audio_gated(sq.key)) continue;   // : gateada = no cubre
            if (!sq.signatures.empty()) {
                if (std::find(sq.signatures.begin(), sq.signatures.end(),
                              e.signature) == sq.signatures.end())
                    continue;
            } else {
                // Subs viejas sin firmas: pertenencia por canal (mismo
                // fallback que el mute de produce_frame).
                const uint32_t bit = chan_bit(e.chip, e.channel);
                if (!(sq.channel_mask & bit)) continue;
            }
            uint32_t anchor = 0;
            if (seq_sub_anchor(sq, f, &anchor)) return true;
        }
        return false;
    }

    /// Máscara de mute dinámico COMPLETA para el frame f — lo MISMO que arma
    /// produce_frame (subs por evento y por Secuencia + instrumento +
    /// ocurrencia + manual) pero SIN disparos ni active_subs. Para los frames
    /// BARE del fast-forward, cuyo PCM se CONSERVA (catch-up del playback,
    /// ) y salía SIN mute: la Secuencia deshabilitada/sustituida se
    /// escuchaba A PLENO en cada ráfaga de deuda (reporte 2026-07-23).
    uint32_t dynamic_audio_mute_at(uint32_t f) {
        uint32_t mute = 0;
        auto chbit = [](const AytherAudioEvent& e) {
            return chan_bit(e.chip, e.channel);
        };
        // : el mute por sustitución exige el asset LISTO — un HD roto en
        // los frames bare del catch-up dejaría huecos de silencio que el
        // camino con produce ya no deja.
        if (audio_sub_preview && !audio_event_assign.empty())
            for (const auto& e : audio_events) {
                if (f < e.start_frame || f > e.end_frame) continue;
                //  F3: exacta o por regla (mismo criterio que el disparo).
                uint64_t asig = 0;
                if (!resolve_event_sig(e.signature, e.instrument, e.pitch,
                                       &asig)) continue;
                if (const auto it = audio_event_assign.find(asig);
                    it != audio_event_assign.end() &&
                    hd_can_sound(asig, it->second)) mute |= chbit(e);
            }
        if (audio_sub_preview)
            for (const auto& sq : audio_seq_subs) {
                if (sq.asset.empty() || !hd_can_sound(sq.key, sq.asset)) continue;
                uint32_t anchor = 0;
                if (!seq_sub_anchor(sq, f, &anchor)) continue;
                if (!sq.signatures.empty()) {
                    mute |= event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
                        return std::find(sq.signatures.begin(), sq.signatures.end(),
                                         e.signature) != sq.signatures.end();
                    });
                } else {
                    mute |= sq.channel_mask;
                }
            }
        if (!audio_instrument_mute.empty())
            mute |= event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
                return audio_instrument_mute.count(e.instrument) != 0;
            });
        // Un timbre RE-SINTETIZADO calla su voz original (): si no, se
        // oirían las dos a la vez. Se mutea por EVENTO y no por canal entero,
        // igual que el resto: en el Mega Drive los efectos comparten canales FM
        // con la música, y silenciar el canal completo se llevaría puesto un
        // golpe de espada que caiga en el medio.
        //
        // GATE DEL HD, igual que la Secuencia de arriba. Sin él, apagar los
        // Assets no cambiaba nada: la voz original seguía muteada y el timbre
        // seguía sonando. El modo Original tiene que devolver el juego intacto.
        if (synth_any && audio_sub_preview)
            mute |= event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
                // DAC/ruido: el SF2 no lo cubre, así que su voz original queda.
                return e.pitch != 255 && inst_assign.count(e.instrument) != 0;
            });
        mute |= occurrence_mute_at(f);
        mute |= audio_manual_mute;
        return mute;
    }
    std::vector<AudioSeqSub>          audio_seq_subs;          // sustitución por Secuencia (grupo → 1 HD)
    std::unordered_set<std::string>   audio_prewarmed;         // assets HD ya decodificados (prewarm)
    // Disparos del preview YA hechos (clave → start_frame+1 de la ocurrencia;
    // 0 = nunca): el trigger dispara al ENTRAR a la ventana, no por igualdad
    // exacta con el start — el catch-up del playback (fast-forward ≤32 en
    // replay_seek) corre los intermedios en bare y si el start caía ahí el
    // one-shot se perdía («a veces no suena», 2026-07-23). Se limpian en los
    // saltos no-secuenciales (scrub) para poder re-disparar al re-pasar.
    std::unordered_map<uint64_t, uint32_t> audio_seq_fired;    // key de Secuencia
    std::unordered_map<uint64_t, uint32_t> audio_evt_fired;    // firma per-evento
    std::vector<AytherAudioActiveSub> audio_active_subs;
    uint32_t                          audio_mute_applied = 0;
    PlaneTileOccurrence    plane_tile_occs[kMaxPlaneTileOccs];   // Fase 2 (panel Capas)
    AytherTileSub          tile_subs[kMaxTileSubs];
    AytherSpriteSub        sprite_subs[kMaxSpriteOccs];
    uint8_t                sprite_sub_flips[kMaxSpriteOccs];  // CU-AN-11: flip observado por sub
                                                              // (paralelo; bit0 hflip, bit1 vflip)
    uint8_t                sprite_sub_tint[kMaxSpriteOccs * 3];  // E1 cromático: tinte RGB Q2.6 por sub (64 = 1.0)
    uint8_t                sprite_sub_slot[kMaxSpriteOccs];   // C8: slot SAT del sub (z-order; menor=encima)
    uint8_t                sprite_sub_prio[kMaxSpriteOccs];   // bit de prioridad VDP del sub (título GA: letras pri-1 DELANTE del plano A pri-1)
    // E1: peak-hold de luminancia por paleta (lo más brillante que estuvo cada
    // paleta en la sesión) — la referencia contra la que se normaliza el fade.
    // No se resetea en replay: durante un barrido de la toma acumula el máximo.
    double                 pal_luma_peak[4] = {0, 0, 0, 0};
    AytherSpriteSub        plane_tile_subs[kMaxPlaneTileSubs];   // Fase 2c (overlay HD de fondo)
    uint8_t                plane_tile_flips[kMaxPlaneTileSubs];  // paralelo: bit0 h, bit1 v
    uint8_t                plane_tile_sub_plane[kMaxPlaneTileSubs];  // paralelo: plano del sub (el enlace celda→set NO cruza planos)
    uint8_t                plane_tile_tint[kMaxPlaneTileSubs * 3];   // paralelo: tinte E1 Q2.6 (64 = neutro; sólo sets con ref autorada)
    PlaneCellHit           plane_cells[kMaxPlaneCells];          // celdas visibles (sync viewport↔Capas)
    /// R-3 (): celdas PARCIALES de los bordes (screen x/y negativos por
    /// scroll no alineado a 8). Van a un array LATERAL durante collect() y se
    /// publican como APÉNDICE de plane_cells recién después de la firma del
    /// Cuadro y los matchers (que iteran [0,npick) y no deben cambiar: las
    /// firmas ya autoradas dependen de ese conjunto exacto). Flag bit3 = parcial.
    PlaneCellHit           plane_cells_border[512];
    /// R-5 (): celdas consumidas por los matchers (paralelo a plane_cells
    /// [0,npick)) — insumo de SceneElement.claimed.
    std::vector<uint8_t>   plane_cell_claimed;
    /// R-5 (): la escena publicada en FrameView (siempre — el flag de
    /// convivencia se retiró con los canales de supresión).
    std::vector<SceneElement> scene_elements;
    // Oclusión de primer plano: máscara pantalla (1 byte/px) con los píxeles OPACOS
    // de tiles de plano de ALTA prioridad — el VDP los dibuja SOBRE los sprites de
    // prioridad baja (telones de transición, arcos). El compose la usa para tapar el
    // HD de pose igual que el hardware tapa al original. Inválida (w=h=0) si el
    // frame no tiene celdas hi-pri o el core no expone VRAM/VSRAM.
    std::vector<uint8_t>   plane_hi_opaque;
    uint16_t               plane_hi_w = 0, plane_hi_h = 0;
    AytherAudioSub         audio_subs[kMaxAudioOccs];
    /// : las subs por hash FILTRADAS por disponibilidad — play y mute
    /// consumen esta lista, así lo que no decodifica no silencia su batch.
    AytherAudioSub         audio_subs_ready[kMaxAudioOccs];
    // Scratch de claiming (pose-sets): qué occurrences reclamó un pose-set +
    // las restantes (sin reclamar) que alimentan el resolve per-sprite.
    uint8_t                sprite_claimed[kMaxSpriteOccs];
    AytherSpriteOccurrence sprite_occs_free[kMaxSpriteOccs];

    FrameView view;
    uint64_t  frame_index = 0;

    // Wire a freshly-opened pack into the script env + the three substitutors,
    // then autoload scripts/init.lua if the pack carries one.
    void load_pack_into(AyArchive* p) {
        // /: IDENTIDAD DEL HORNEADO, una vez y al principio del log.
        //
        // Con los assets nombrados por hash, un error dice `a3f9c1…` y no hay
        // forma de saber qué archivo es sin el log del bake que lo produjo. El
        // build id es lo que empareja las dos cosas: se deriva de integrity.toml
        // —el conjunto de hashes de todo lo que hay adentro— así que no puede
        // mentir, y dos horneados idénticos dan el mismo.
        //
        // Vacío = pack legacy (sin integrity.toml). Se dice así en vez de
        // callarlo: «no sé de qué build es» es información, y ocultarla mandaría
        // a buscar en un log que no existe.
        if (p) {
            const char* bid = ayther_pack_build_id(p);
            std::fprintf(stderr, "[pack] %s build %s\n",
                         ayther_pack_game_id(p),
                         (bid && *bid) ? bid : "(legacy, sin build id)");
        }
        // : la fuente PACK de la mejora por software se rearma desde cero
        // con cada pack (la fuente Lab, set_enhanced_elements, no se toca).
        for (auto& s : element_enhance_pack) s.clear();
        rebuild_enhance_sets();
        ayther_script_set_pack    (script.get(), p);
        ayther_tile_sub_load_pack (tile_sub.get(),   p);
        ayther_tile_sub_load_pack_named(plane_sub.get(), p, "plane_tile_substitutions.toml");
        ayther_sprite_sub_load_pack(sprite_sub.get(), p);
        ayther_pose_sub_load_pack(pose_sub.get(), p);   // CU-AN multi-sprite
        ayther_tween_load_pack(tween.get(), p);         // CU-AN in-betweens
        ayther_audio_sub_load_pack(audio_sub.get(),  p);

        const int64_t sz = ayther_pack_file_size(p, "scripts/init.lua");
        if (sz > 0) {
            std::vector<uint8_t> src(static_cast<size_t>(sz) + 1, 0);
            ayther_pack_read(p, "scripts/init.lua", src.data(), static_cast<size_t>(sz));
            ayther_script_load_string(
                script.get(), reinterpret_cast<const char*>(src.data()), "scripts/init.lua");
        }

        // Componentes: animations.toml (AnimationPlayer) + audio_events.toml
        // (mirror de asignaciones por evento; el core Rust parsea el mismo
        // archivo hacia el catálogo del sub de batches). define()/assign()
        // reemplazan por id → recargar el pack es idempotente y lo autorado
        // sin hornear sobrevive.
        auto read_text = [&](const char* name) -> std::string {
            const int64_t n = ayther_pack_file_size(p, name);
            if (n <= 0) return {};
            std::string s(static_cast<size_t>(n), '\0');
            ayther_pack_read(p, name, reinterpret_cast<uint8_t*>(s.data()), s.size());
            return s;
        };
        if (const std::string t = read_text("animations.toml"); !t.empty())
            parse_animations_toml(t, anim);
        if (const std::string t = read_text("audio_events.toml"); !t.empty())
            parse_audio_events_toml(t, audio_evt);

        //  EM-8.2: el gate del ENSANCHADO. Lo compila el CORE (camino A,
        // igual que el de audio): un segundo evaluador del dialecto de
        // condiciones se desincronizaría, y el autor tendría que aprender dos.
        //
        // Sin `widescreen.toml` el gate queda NULL y el ancho pedido manda tal
        // cual — que es lo que hace que todos los packs ya horneados y el
        // ensanchado manual del Lab sigan funcionando sin tocar nada.
        wide_gate.reset();
        if (const std::string t = read_text("widescreen.toml"); !t.empty())
            wide_gate.reset(ayther_widescreen_gate_new(t.c_str()));

        // : instruments.toml — la re-síntesis POR TIMBRE del pack. El
        // parser del core lo lee para otras cosas (soundfonts_used), pero
        // nadie lo traía a la sesión: en el Lab el catálogo llega del frontend
        // cada frame, y en Play NO LLEGA NADIE, así que la re-síntesis no
        // sonaba nunca. Mismo lector lineal naive que el resto de nuestros
        // TOML: el dialecto es el que escribe el proyecto, sin traducción.
        inst_assign_pack.clear();
        if (const std::string t = read_text("instruments.toml"); !t.empty()) {
            std::vector<PackInstrument> insts;
            parse_instruments_toml(t, insts);
            for (const PackInstrument& pi : insts) {
                InstAssign as;
                as.soundfont = pi.soundfont;
                as.bank      = pi.bank;
                as.preset    = pi.preset;
                as.transpose = pi.transpose;
                as.gain      = pi.gain;
                inst_assign_pack[pi.patch] = as;
            }
        }

        // : el Modo 3 viaja en el pack — game_profile.toml (anclas + gate,
        // derivado del modelo de Maper al hornear) + entity_substitutions.toml
        // (kind → asset HD por instancia, referenciado por nombre de entrada
        // del pack como los sprite subs). Un pack SIN perfil no toca el
        // resolver: lo que la autoría viva haya cargado/asignado sobrevive.
        // : los ACETATOS del pack. Se leen acá y se guardan; el stack lo
        // arma el frontend (ver pack_overlays() en el header). Mismo lector
        // lineal naive que el resto de nuestros TOML — el dialecto es el que
        // escribe el proyecto, sin traducción.
        overlays.clear();
        if (const std::string t = read_text("acetatos.toml"); !t.empty()) {
            auto quoted = [](const std::string& line) -> std::string {
                const size_t a = line.find('"');
                const size_t b = line.rfind('"');
                if (a == std::string::npos || b <= a) return {};
                return line.substr(a + 1, b - a - 1);
            };
            auto num = [](const std::string& line) -> double {
                const size_t eq = line.find('=');
                return eq == std::string::npos ? 0.0
                                               : std::atof(line.c_str() + eq + 1);
            };
            auto field = [](const std::string& line, const char* key) {
                // "opacity = 1" vs "opacity_x": exigir el separador evita que un
                // campo nuevo con prefijo común se coma al viejo.
                const size_t n = std::strlen(key);
                return line.compare(0, n, key) == 0 && line.size() > n &&
                       (line[n] == ' ' || line[n] == '=');
            };
            AytherSession::PackOverlay cur;
            bool open = false;
            size_t pos = 0;
            while (pos <= t.size()) {
                size_t nl = t.find('\n', pos);
                if (nl == std::string::npos) nl = t.size();
                std::string line = t.substr(pos, nl - pos);
                const bool eof = nl >= t.size();
                pos = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.compare(0, 11, "[[acetato]]") == 0) {
                    if (open) overlays.push_back(std::move(cur));
                    cur = AytherSession::PackOverlay{};
                    open = true;
                } else if (!open) {
                    // comentario de cabecera
                } else if (field(line, "name"))    cur.name = quoted(line);
                else if (field(line, "visible"))
                    cur.visible = line.find("true") != std::string::npos;
                else if (field(line, "asset")) {
                    const std::string a = quoted(line);
                    std::snprintf(cur.content.asset, sizeof(cur.content.asset),
                                  "%s", a.c_str());
                } else if (field(line, "img_w"))
                    cur.content.img_w = uint16_t(num(line));
                else if (field(line, "img_h"))
                    cur.content.img_h = uint16_t(num(line));
                else if (field(line, "y"))
                    cur.content.y = int16_t(num(line));
                else if (field(line, "anchor"))
                    cur.content.anchor = uint8_t(num(line));
                else if (field(line, "factor"))
                    cur.content.factor = float(num(line));
                else if (field(line, "opacity"))
                    cur.content.opacity = float(num(line));
                else if (field(line, "blend"))
                    cur.content.blend = uint8_t(num(line));
                else if (field(line, "tile_mode"))
                    cur.content.tile_mode = uint8_t(num(line));
                else if (field(line, "drift_x"))
                    cur.content.drift_x = float(num(line));
                else if (field(line, "drift_y"))
                    cur.content.drift_y = float(num(line));
                // : lo que el lector IGNORABA. La estructura ya tenia los
                // campos —AytherLayerContent los declara todos— asi que lo
                // unico que faltaba era leerlos: se autoraban, se horneaban y
                // se perdian al abrir el pack.
                else if (field(line, "fit"))
                    cur.content.fit = uint8_t(num(line));
                else if (field(line, "flicker_amp"))
                    cur.content.flicker_amp = float(num(line));
                else if (field(line, "flicker_ticks"))
                    cur.content.flicker_ticks = uint16_t(num(line));
                else if (field(line, "pal"))
                    cur.content.pal_line = uint8_t(num(line));
                else if (field(line, "ref")) {
                    // "r,g,b" en decimal, como lo escribe el writer.
                    const std::string v = quoted(line);
                    int c[3] = {0, 0, 0};
                    std::sscanf(v.c_str(), "%d,%d,%d", &c[0], &c[1], &c[2]);
                    for (int k = 0; k < 3; ++k)
                        cur.content.ref_rgb[k] =
                            uint8_t(c[k] < 0 ? 0 : (c[k] > 255 ? 255 : c[k]));
                } else if (field(line, "tint_mask"))
                    cur.content.tint_mask = uint16_t(num(line));
                else if (field(line, "screen")) {
                    // Pipe-separated hexadecimal ids. When the fixed wire
                    // capacity is reached, retain the accepted prefix rather
                    // than disabling the gate completely.
                    const std::string v = quoted(line);
                    size_t b = 0;
                    while (b < v.size()) {
                        size_t e = v.find('|', b);
                        if (e == std::string::npos) e = v.size();
                        const std::string tok = v.substr(b, e - b);
                        const uint64_t id = tok.empty()
                            ? 0
                            : std::strtoull(tok.c_str(), nullptr, 0);
                        if (id != 0 && !cur.content.add_screen(id)) break;
                        b = e + 1;
                    }
                } else if (field(line, "gate"))
                    cur.content.gate_presence =
                        quoted(line) == "presencia" ? 1 : 0;
                else if (field(line, "frames")) {
                    // Pasos EXTRA de la animacion: el primero es el `asset`.
                    const std::string v = quoted(line);
                    size_t b = 0;
                    uint8_t k = 0;
                    while (b < v.size() && k < 3) {
                        size_t e = v.find('|', b);
                        if (e == std::string::npos) e = v.size();
                        const std::string tok = v.substr(b, e - b);
                        if (!tok.empty()) {
                            std::snprintf(cur.content.anim[k],
                                          sizeof(cur.content.anim[k]),
                                          "%s", tok.c_str());
                            ++k;
                        }
                        b = e + 1;
                    }
                    cur.content.anim_count = k;
                } else if (field(line, "ticks"))
                    cur.content.anim_ticks = uint16_t(num(line));
                if (eof) break;
            }
            if (open) overlays.push_back(std::move(cur));
        }

        if (const std::string t = read_text("game_profile.toml"); !t.empty()) {
            if (auto r = mode3.load_profile_str(t); !r) {
                std::fprintf(stderr, "[pack] game_profile.toml inválido: %s\n",
                             r.error.message.c_str());
            } else if (const std::string es =
                           read_text("entity_substitutions.toml");
                       !es.empty()) {
                // [[sub]] kind = "player" / asset = "<entrada del pack>" —
                // el mismo lector lineal naive de los demás TOML nuestros.
                auto quoted = [](const std::string& line) -> std::string {
                    const size_t a = line.find('"');
                    const size_t b = line.rfind('"');
                    if (a == std::string::npos || b <= a) return {};
                    return line.substr(a + 1, b - a - 1);
                };
                std::string kind;
                size_t      pos = 0;
                while (pos < es.size()) {
                    size_t nl = es.find('\n', pos);
                    if (nl == std::string::npos) nl = es.size();
                    const std::string line = es.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (line.find("kind") == 0)
                        kind = quoted(line);
                    else if (line.find("asset") == 0 && !kind.empty()) {
                        mode3.assign_kind(kind, quoted(line));
                        kind.clear();
                    }
                }
            }
        }
        // : las Identidades autorables de Pintar —Cuadro, Panorámica,
        // Cinemática, Animación, Utilería, Carácter, UI— viajan en UN
        // documento. Antes eran cinco archivos separados por el MECANISMO del
        // motor que las sirve, que es un detalle de implementación decidiendo
        // el layout de la entrega.
        //
        // Un pack VIEJO trae los cinco por separado y se leen igual: el
        // documento único es la concatenación de los mismos arrays, así que los
        // decodificadores son los mismos y sólo cambia de dónde salen los bytes.
        std::vector<PackScreen>        pk_scr;
        std::vector<PackPanorama>      pk_pans;
        std::vector<PackKinematic>     pk_kins;
        std::vector<PackPlaneSequence> pk_seqs;
        std::vector<PackPlaneSet>      pk_sets;
        std::vector<PackPlaneFont>     pk_fonts;
        std::vector<PackEnhance>       pk_enh;     // 
        if (const std::string t = read_text("elements.toml"); !t.empty()) {
            parse_elements_toml(t, pk_scr, pk_pans, pk_kins, pk_seqs,
                                pk_sets, pk_fonts, &pk_enh);
        } else {
            if (const std::string t2 = read_text("plane_sets.toml"); !t2.empty())
                parse_plane_sets_toml(t2, pk_sets, pk_fonts);
            if (const std::string t2 = read_text("screens.toml"); !t2.empty())
                parse_screens_toml(t2, pk_scr);
            if (const std::string t2 = read_text("panoramas.toml"); !t2.empty())
                parse_panoramas_toml(t2, pk_pans);
            if (const std::string t2 = read_text("kinematics.toml"); !t2.empty())
                parse_kinematics_toml(t2, pk_kins);
            if (const std::string t2 = read_text("plane_sequences.toml"); !t2.empty())
                parse_plane_sequences_toml(t2, pk_seqs);
        }

        // : «Mejorar por software» — ya viene expandido a (capa, hash);
        // el inventario lo publica como fx_enhance y el compose lo aplica.
        // No pasa por el matcher ni por el resolver de assets.
        if (!pk_enh.empty()) {
            size_t n = 0;
            for (const PackEnhance& e : pk_enh) {
                if (e.layer > 3) continue;
                for (uint64_t h : e.hashes) { element_enhance_pack[e.layer][h] = e.k; ++n; }
            }
            rebuild_enhance_sets();
            std::fprintf(stderr, "[pack] mejora por software: %zu identidad(es), %zu (capa,hash)\n",
                         pk_enh.size(), n);
        }

        // Utilería (CU002) y Glifos (CU005): el catálogo multi-tile de Pintar.
        // Hasta que este archivo existió, los sets sólo vivían en la sesión de
        // autoría (inyectados por API) y el pack entregado NO reproducía
        // ninguna sustitución multi-tile de plano.
        {
            for (const PackPlaneSet& s : pk_sets) {
                Impl::PlaneSetDef d;
                d.plane   = s.plane;
                d.w_cells = s.w_cells;
                d.h_cells = s.h_cells;
                d.asset   = s.asset;
                std::memcpy(d.ref_rgb, s.ref_rgb, 3);   // tinte E1 (0,0,0 = sin)
                d.members.reserve(s.members.size());
                for (const PackPlaneSetMember& m : s.members)
                    d.members.push_back({ m.hash, m.cx, m.cy });
                d.off_x = s.off_x;   // 
                d.off_y = s.off_y;
                plane_sets[s.id] = std::move(d);   // por id → recargar es idempotente
            }
        }
        // CUADROS (CU001): pantallas estáticas completas.
        {
            for (const PackScreen& sc : pk_scr) {
                ScreenDef d;
                d.mask      = sc.plane_mask ? sc.plane_mask : 0x07;
                d.min_match = sc.min_match;
                d.max_extra = sc.max_extra;
                d.asset     = sc.asset;
                d.cells.reserve(sc.cells.size());
                for (const PackScreenCell& c : sc.cells) {
                    if (c.plane > 2 || !(d.mask & (1u << c.plane))) continue;
                    d.cells.emplace(((uint32_t)c.plane << 24)
                                    | ((uint32_t)c.col << 8) | c.row, c.hash);
                    uint64_t x = c.hash;
                    x ^= (uint64_t)c.col * 0x9E3779B97F4A7C15ull;
                    x ^= (uint64_t)c.row * 0xC2B2AE3D27D4EB4Full;
                    x ^= (uint64_t)(c.plane + 1) * 0x165667B19E3779F9ull;
                    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 29;
                    d.sig_plane[c.plane] += x;      // por CAPA, no sumadas
                    ++d.cells_plane[c.plane];
                    d.hashes_plane[c.plane].insert(c.hash);
                }
                if (!d.cells.empty()) screens[sc.id] = std::move(d);
            }
        }

        // PANORÁMICAS (CU003): la tira del nivel de una capa. Sin esto, una
        // panorámica autorada vivía sólo en la sesión del Lab — el pack
        // entregado no la reproducía, que es el mismo agujero que tenían la
        // Utilería y los Glifos antes de plane_sets.toml.
        {
            for (const PackPanorama& p : pk_pans) {
                std::vector<AytherSession::PanoramaCell> cs;
                cs.reserve(p.cells.size());
                for (const PackPanoramaCell& c : p.cells)
                    cs.push_back({ c.hash, c.lx, c.ly });
                if (cs.empty()) continue;
                panoramas[p.id] = build_panorama(p.plane, p.origin_x, p.origin_y,
                                                 p.w_cells, p.h_cells,
                                                 cs.data(), (uint32_t)cs.size(),
                                                 p.asset);
            }
        }

        // CINEMÁTICAS (CU004): la secuencia ordenada de Cuadros. Va DESPUÉS de
        // screens.toml a propósito — sus pasos apuntan a esos ids.
        if (!pk_kins.empty()) {
            for (const PackKinematic& k : pk_kins) {
                if (k.steps.size() < 2) continue;   // un paso = un Cuadro
                KinematicDef d;
                d.gap   = k.gap_frames;
                d.loop      = k.loop;
                d.audio     = k.audio;
                d.gain      = k.gain;
                d.game_gain = k.game_gain;
                d.steps.reserve(k.steps.size());
                d.assets.reserve(k.steps.size());
                d.video_offsets.reserve(k.steps.size());
                for (const PackKinematicStep& s : k.steps) {
                    d.steps.push_back(s.screen_id);
                    d.assets.push_back(s.asset);
                    d.video_offsets.push_back(s.video_offset);
                }
                kinematics[k.id] = std::move(d);
            }
            kinematic_reindex();
            kinematic_reset();
        }

        // ANIMACIONES (): secuencias de Objetos con reloj propio. DESPUÉS
        // de plane_sets.toml — sus pasos apuntan a esos ids.
        {
            for (const PackPlaneSequence& q : pk_seqs) {
                if (q.steps.size() < 2) continue;
                PlaneSeqDef d;
                d.steps.reserve(q.steps.size());
                d.assets.reserve(q.steps.size());
                d.durs.reserve(q.steps.size());
                for (const PackPlaneSeqStep& s : q.steps) {
                    d.steps.push_back(s.set_id);
                    d.assets.push_back(s.asset);
                    d.durs.push_back(s.duration);
                    d.total += s.duration ? s.duration : kSeqDefaultDur;
                }
                plane_seqs[q.id] = std::move(d);
            }
            plane_seq_reindex();
            seq_clocks.clear();
        }

        // : y por último los timbres horneados. Va al FINAL para que
        // corra con el pack ya montado: `load_sf2_shared` busca el SF2 en el
        // pack antes que en disco.
        apply_pack_instruments();
    }

    /// : aplica los timbres de `instruments.toml` del pack.
    ///
    /// Sólo si NADIE autoró: el frontend manda su catálogo entero en cada
    /// vuelta (`set_instrument_assigns`), así que si el Lab tiene timbres
    /// asignados, esos ganan — mismo criterio que la unión Lab u pack de
    /// . En Play y en el runtime no hay quien mande, y ahí es donde esto
    /// hace la diferencia entre que la re-sintesis suene o no exista.
    void apply_pack_instruments() {
        if (inst_assign_pack.empty() || !inst_assign.empty()) return;
        for (const auto& [patch, as] : inst_assign_pack) {
            if (!synths.count(patch)) {
                AytherSf2* sy = load_sf2_shared(as.soundfont);
                if (!sy)
                    std::fprintf(stderr,
                        "[sf2] pack: no se pudo cargar '%s' — ese timbre "
                        "suena con su chip\n", as.soundfont.c_str());
                // Se cachea AUNQUE sea nulo, igual que en el camino del
                // frontend: sin esto se reintentaria por cada asignacion.
                synths[patch] = sy;
            }
            inst_assign[patch] = as;
            synth_any = true;
        }
        // Preset y REALCE por timbre, igual que set_instrument_assigns: la
        // atenuacion (<1) va por CC 7 en el note_on; el realce (>1) escala el
        // render de ESE sintetizador.
        for (auto& [inst, as] : inst_assign) {
            if (AytherSf2* sy = synth_for(inst)) {
                ayther_sf2_program(sy, 0, as.preset);
                if (as.gain > 1.0f) synth_boost[inst] = as.gain;
            }
        }
        ayther_sf2_trim_cache();
    }
};
