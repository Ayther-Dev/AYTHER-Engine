#pragma once
// ---------------------------------------------------------------------------
// ayther_config.h — persistent user configuration for Ayther Engine + Lab.
//
// Storage: %APPDATA%\Ayther\config.toml  (Windows)
//          ~/.config/Ayther/config.toml   (Linux / macOS — future)
//
// Created automatically on first run with sensible defaults.
// The same config instance is shared between the engine (launcher, ROM paths)
// and the Lab (projects directory).  Pass a pointer via ILabPlugin::set_config.
//
// Call sequence:
//   AytherConfig config = AytherConfig::load();
//   // ... use config.rom_library etc. ...
//   config.push_recent(rom_path);
//   config.save();
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>
#include <vector>

struct AytherConfig {
    // ---- Paths -------------------------------------------------------------

    /// Root folder used as the starting directory for ROM file pickers.
    std::string rom_library;

    /// Default folder for .ay pack files (used by launcher and Lab).
    std::string packs_dir;

    /// Folder containing libretro core DLLs / SOs.
    std::string cores_dir;

    /// Root folder for Ayther Lab projects (each project is a sub-directory).
    std::string projects_dir;

    /// Full path to the libretro core the Lab launches sessions with (the
    /// VRAM-exposing fork). Empty until the user configures it.
    std::string core_path;

    /// No-Intro DAT (opcional, lo provee el usuario — no se distribuye):
    /// nombra canónicamente los ROMs por CRC32 al crear proyectos.
    std::string dat_path;

    /// Colección de SoundFonts (.sf2/.sf3/.sfz) del usuario — la biblioteca de la
    /// re-síntesis de timbres (). Va en la config GLOBAL y no en el
    /// proyecto porque una colección se junta una vez y sirve para todos: la
    /// del usuario tiene 182 archivos y no tendría sentido re-elegirla por
    /// proyecto. Lo asignado sí es del proyecto (instruments.toml), y al
    /// hornear el pack viajan sólo los presets usados — así que el .ay no
    /// depende de esta carpeta.
    std::string soundfonts_dir;

    /// Definiciones por juego del Maper (defs/<game_id>.toml: direcciones RAM
    /// + stages, keyed por CRC32 No-Intro). Global y cross-proyecto.
    /// Default: %APPDATA%\Ayther\defs.
    std::string defs_dir;

    /// RetroAchievements (importador de Code Notes del Maper, M5). La API
    /// key es del usuario (retroachievements.org → Settings → API Key).
    std::string ra_user;
    std::string ra_api_key;

    /// Servidor MCP embebido del Lab (plan §7): expone tools para agentes de
    /// IA por HTTP en localhost. OPT-IN, apagado por defecto.
    bool mcp_enabled = false;
    int  mcp_port    = 7777;

    // ---- Recent ROMs -------------------------------------------------------

    static constexpr int kMaxRecent = 10;

    /// Ordered list of recently opened ROM paths (most-recent first).
    std::vector<std::string> recent_roms;

    /// Recently opened Lab project directories (most-recent first) — drives
    /// the launcher's project grid.
    std::vector<std::string> recent_projects;

    // ---- Input bindings (Lab Capturar: P1 6 botones; P2 habilitable) --------

    /// Acciones del pad Genesis de 6 botones, por jugador.
    enum class GameAction : int {
        DpadUp = 0, DpadDown, DpadLeft, DpadRight,
        A, B, C, X, Y, Z, Start, Mode,
        Count
    };
    /// Controles de captura (globales): un toggle de grabación + marcar.
    enum class LabAction : int { RecordToggle = 0, Mark, Count };

    static constexpr int kGameActions   = static_cast<int>(GameAction::Count);
    static constexpr int kLabActions = static_cast<int>(LabAction::Count);

    /// Bindings por jugador. Teclado: nombres de SDL_GetScancodeName ("Up",
    /// "Z"); gamepad: SDL_GetGamepadStringForButton ("a", "dpup",
    /// "leftshoulder"). "" = sin asignar.
    struct InputBindings {
        std::string kb [kGameActions];
        std::string pad[kGameActions];
    };

    InputBindings input_p1, input_p2;
    bool          p2_enabled = false;            ///< habilita el puerto 2
    std::string   lab_kb [kLabActions];    ///< Grabar/Detener · Marcar
    std::string   lab_pad[kLabActions];

    /// Botones del mando para Aceptar/Descartar en los diálogos de nombre
    /// (Grabar/Marcar). Calibración press-to-bind: guardan la POSICIÓN física
    /// (nombre SDL: "a"=south, "b"=east, …), no el rótulo — robusto contra
    /// mandos que SDL mal-rotula. "" = autodetección por tipo de mando.
    std::string   dialog_accept_pad;
    std::string   dialog_discard_pad;

    /// Tipo de mando POR JUGADOR (0 = Xbox · 1 = Nintendo · 2 = PlayStation).
    /// Solo afecta la PRESENTACIÓN de los botones (el A de Nintendo no está
    /// donde el de Xbox; PlayStation usa formas) y la resolución "(auto)" de
    /// Aceptar/Descartar en diálogos. Los bindings guardan SIEMPRE el nombre
    /// SDL posicional ("a"=south, …), estable entre tipos.
    int pad_type[2] = {0, 0};

    /// Claves TOML estables por acción ("dpad_up", "a", … / "record", …).
    static const char* game_action_key(GameAction a);
    static const char* lab_action_key(LabAction a);

    // ---- Lab behaviour -----------------------------------------------------

    /// When true, the Lab auto-saves the current project when its window
    /// loses focus (SDL_EVENT_WINDOW_FOCUS_LOST).
    bool auto_save = false;

    // ---- Persistence -------------------------------------------------------

    /// Load from %APPDATA%\Ayther\config.toml.
    /// Creates the directory and file with defaults if they don't exist.
    static AytherConfig load();

    /// Write the current state to %APPDATA%\Ayther\config.toml.
    void save() const;

    // ---- Recent ROM management ---------------------------------------------

    /// Prepend rom_path, remove any duplicate further down the list,
    /// and cap the list at kMaxRecent entries.
    void push_recent(const std::string& rom_path);

    /// Same policy, for recent_projects.
    void push_recent_project(const std::string& project_dir);

    // ---- Path helpers ------------------------------------------------------

    /// %APPDATA%\Ayther\  (or platform equivalent — created lazily by load()).
    static std::filesystem::path config_dir();

    /// config_dir() / "config.toml"
    static std::filesystem::path config_file();

private:
    /// Fill fields with defaults based on the current environment.
    void apply_defaults();
};
