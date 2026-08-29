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

    /// No-Intro DAT (optional, supplied by the user — not distributed):
    /// canonically names ROMs by CRC32 when creating projects.
    std::string dat_path;

    /// The user's SoundFont collection (.sf2/.sf3/.sfz) — the library behind
    /// timbre re-synthesis. It lives in the GLOBAL config and not in the
    /// project because a collection is gathered once and serves them all: this
    /// user's holds 182 files and re-picking it per project would make no
    /// sense. What is assigned IS per project (instruments.toml), and when the
    /// pack is baked only the presets in use travel with it — so the .ay does
    /// not depend on this folder.
    std::string soundfonts_dir;

    /// Per-game Mapper definitions (defs/<game_id>.toml: RAM addresses +
    /// stages, keyed by No-Intro CRC32). Global and cross-project.
    /// Default: %APPDATA%\Ayther\defs.
    std::string defs_dir;

    /// RetroAchievements (the Mapper's Code Notes importer, M5). The API key
    /// belongs to the user (retroachievements.org → Settings → API Key).
    std::string ra_user;
    std::string ra_api_key;

    /// The Lab's embedded MCP server (plan §7): exposes tools for AI agents
    /// over HTTP on localhost. OPT-IN, off by default.
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

    /// Actions of the 6-button Genesis pad, per player.
    enum class GameAction : int {
        DpadUp = 0, DpadDown, DpadLeft, DpadRight,
        A, B, C, X, Y, Z, Start, Mode,
        Count
    };
    /// Capture controls (global): a recording toggle + mark.
    enum class LabAction : int { RecordToggle = 0, Mark, Count };

    static constexpr int kGameActions   = static_cast<int>(GameAction::Count);
    static constexpr int kLabActions = static_cast<int>(LabAction::Count);

    /// Per-player bindings. Keyboard: SDL_GetScancodeName names ("Up", "Z");
    /// gamepad: SDL_GetGamepadStringForButton ("a", "dpup", "leftshoulder").
    /// "" = unassigned.
    struct InputBindings {
        std::string kb [kGameActions];
        std::string pad[kGameActions];
    };

    InputBindings input_p1, input_p2;
    bool          p2_enabled = false;            ///< enables port 2
    std::string   lab_kb [kLabActions];    ///< Record/Stop · Mark
    std::string   lab_pad[kLabActions];

    /// Controller buttons for Accept/Discard in the naming dialogs
    /// (Record/Mark). Press-to-bind calibration: they store the physical
    /// POSITION (SDL name: "a"=south, "b"=east, …), not the label — robust
    /// against controllers SDL mislabels. "" = auto-detect from controller
    /// type.
    std::string   dialog_accept_pad;
    std::string   dialog_discard_pad;

    /// Controller type PER PLAYER (0 = Xbox · 1 = Nintendo · 2 = PlayStation).
    /// It affects only the PRESENTATION of the buttons (Nintendo's A is not
    /// where Xbox's is; PlayStation uses shapes) and the "(auto)" resolution of
    /// Accept/Discard in dialogs. Bindings ALWAYS store the positional SDL name
    /// ("a"=south, …), which is stable across types.
    int pad_type[2] = {0, 0};

    /// Stable per-action TOML keys ("dpad_up", "a", … / "record", …).
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
