// ---------------------------------------------------------------------------
// ayther_config.cpp — AytherConfig implementation
// ---------------------------------------------------------------------------

#include "ayther_config.h"
#include "log.h"
#include "ayther_env.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

// ---------------------------------------------------------------------------
// Platform path helpers
// ---------------------------------------------------------------------------

namespace {

/// Returns the user's home directory (USERPROFILE on Windows, HOME elsewhere).
std::string home_dir() {
#ifdef _WIN32
    const char* p = ayther::env_get("USERPROFILE");
#else
    const char* p = ayther::env_get("HOME");
#endif
    return p ? std::string(p) : ".";
}

/// Returns the user's roaming app-data directory.
///   Windows : %APPDATA%     → C:\Users\<user>\AppData\Roaming
///   Others  : ~/.config
std::string appdata_dir() {
#ifdef _WIN32
    const char* p = ayther::env_get("APPDATA");
    return p ? std::string(p) : home_dir();
#else
    const char* p = ayther::env_get("XDG_CONFIG_HOME");
    if (p && *p) return std::string(p);
    return (std::filesystem::path(home_dir()) / ".config").string();
#endif
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Input action keys
// ---------------------------------------------------------------------------

const char* AytherConfig::game_action_key(GameAction a) {
    static const char* k[kGameActions] = {
        "dpad_up", "dpad_down", "dpad_left", "dpad_right",
        "a", "b", "c", "x", "y", "z", "start", "mode",
    };
    return k[static_cast<int>(a)];
}

const char* AytherConfig::lab_action_key(LabAction a) {
    static const char* k[kLabActions] = { "record", "mark" };
    return k[static_cast<int>(a)];
}

// ---------------------------------------------------------------------------
// Path helpers (public)
// ---------------------------------------------------------------------------

std::filesystem::path AytherConfig::config_dir() {
    return std::filesystem::path(appdata_dir()) / "Ayther";
}

std::filesystem::path AytherConfig::config_file() {
    return config_dir() / "config.toml";
}

// ---------------------------------------------------------------------------
// apply_defaults — fill any empty fields with sensible starting values
// ---------------------------------------------------------------------------

void AytherConfig::apply_defaults() {
    namespace fs = std::filesystem;

    if (rom_library.empty()) {
#ifdef _WIN32
        const char* docs = ayther::env_get("USERPROFILE");
        rom_library = docs ? (fs::path(docs) / "Documents").string() : home_dir();
#else
        rom_library = home_dir();
#endif
    }

    if (projects_dir.empty()) {
#ifdef _WIN32
        const char* docs = ayther::env_get("USERPROFILE");
        auto base = docs ? fs::path(docs) / "Documents" : fs::path(home_dir());
#else
        auto base = fs::path(home_dir()) / "Documents";
#endif
        // Rebrand 2026-07-25: si el usuario ya tiene la carpeta legacy, se
        // sigue usando (no se le mueven los proyectos); el nombre nuevo es
        // solo el default de instalaciones frescas.
        std::error_code pec;
        const fs::path legacy = base / "AetherProjects";
        projects_dir = fs::is_directory(legacy, pec)
                           ? legacy.string()
                           : (base / "AytherProjects").string();
    }

    if (defs_dir.empty())
        defs_dir = (config_dir() / "defs").string();

    // packs_dir and cores_dir are left empty until the user configures them
    // (the launcher will prompt on first run).

    // ---- Default bindings (P1 + Capturar) — only when everything is empty ---
    auto all_empty = [](const std::string* arr, int n) {
        for (int i = 0; i < n; ++i) if (!arr[i].empty()) return false;
        return true;
    };
    if (all_empty(input_p1.kb, kGameActions)) {
        const char* kb[kGameActions] = {
            "Up", "Down", "Left", "Right",       // D-Pad
            "Z", "X", "C",                       // Genesis A/B/C
            "A", "S", "D",                       // Genesis X/Y/Z
            "Return", "Right Shift",             // Start / Mode
        };
        for (int i = 0; i < kGameActions; ++i) input_p1.kb[i] = kb[i];
    }
    if (all_empty(input_p1.pad, kGameActions)) {
        const char* pad[kGameActions] = {
            "dpup", "dpdown", "dpleft", "dpright",
            "x", "a", "b",                       // Genesis A/B/C (layout xbox)
            "leftshoulder", "y", "rightshoulder",// Genesis X/Y/Z
            "start", "back",                     // Start / Mode
        };
        for (int i = 0; i < kGameActions; ++i) input_p1.pad[i] = pad[i];
    }
    if (all_empty(lab_kb, kLabActions)) {
        lab_kb[0] = "F9";      // Grabar/Detener (toggle REC)
        lab_kb[1] = "F10";     // Marcar
    }
    // lab_pad y P2 quedan sin asignar hasta que el usuario los configure.
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

AytherConfig AytherConfig::load() {
    namespace fs = std::filesystem;

    AytherConfig cfg;
    cfg.apply_defaults();

    const fs::path dir  = config_dir();
    const fs::path file = config_file();

    // Ensure the config directory exists.
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        ayther::log::write(ayther::log::Severity::Error,
            "config", "cannot_create_config_dir",
            "Cannot create config dir %s: %s",
            dir.string().c_str(),
            ec.message().c_str());
        return cfg;  // return defaults
    }

    if (!fs::exists(file)) {
        // Migración del rebrand (2026-07-25): si existe la config legacy en
        // <appdata>/Aether, copiarla completa (config.toml + defs/) al dir
        // nuevo y seguir con ella — el usuario no pierde rutas ni bindings.
        const fs::path legacy_dir = fs::path(appdata_dir()) / "Aether";
        if (fs::exists(legacy_dir / "config.toml", ec)) {
            fs::copy(legacy_dir, dir,
                     fs::copy_options::recursive |
                         fs::copy_options::skip_existing, ec);
            ayther::log::write(ayther::log::Severity::Info,
                "config", "migrada_config_legacy",
                "migrada config legacy %s -> %s",
                legacy_dir.string().c_str(),
                dir.string().c_str());
        }
    }
    if (!fs::exists(file)) {
        // First run — persist the defaults so the file is present.
        cfg.save();
        return cfg;
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(file.string());
    } catch (const toml::parse_error& e) {
        ayther::log::write(ayther::log::Severity::Error,
            "config", "parse_error_using_defaults",
            "Parse error in %s: %s — using defaults",
            file.string().c_str(),
            e.description().data());
        return cfg;
    }

    auto get_str = [&](const char* key, std::string& out) {
        if (auto v = tbl[key].value<std::string>(); v && !v->empty())
            out = *v;
    };

    get_str("rom_library",  cfg.rom_library);
    get_str("packs_dir",    cfg.packs_dir);
    get_str("cores_dir",    cfg.cores_dir);
    get_str("projects_dir", cfg.projects_dir);
    get_str("core_path",    cfg.core_path);
    get_str("dat_path",     cfg.dat_path);
    get_str("soundfonts_dir", cfg.soundfonts_dir);
    get_str("defs_dir",     cfg.defs_dir);
    get_str("ra_user",      cfg.ra_user);
    get_str("ra_api_key",   cfg.ra_api_key);
    get_str("dialog_accept_pad",  cfg.dialog_accept_pad);
    cfg.pad_type[0] = (int)tbl["pad_type_p1"].value_or<int64_t>(0);
    cfg.pad_type[1] = (int)tbl["pad_type_p2"].value_or<int64_t>(0);
    get_str("dialog_discard_pad", cfg.dialog_discard_pad);
    cfg.auto_save   = tbl["auto_save"].value_or(false);
    cfg.mcp_enabled = tbl["mcp_enabled"].value_or(false);
    cfg.mcp_port    = (int)tbl["mcp_port"].value_or<int64_t>(7777);

    auto get_list = [&](const char* key, std::vector<std::string>& out) {
        if (auto* arr = tbl[key].as_array()) {
            out.clear();
            for (const auto& node : *arr) {
                if (auto v = node.value<std::string>(); v && !v->empty())
                    out.push_back(*v);
            }
        }
    };
    get_list("recent_roms",     cfg.recent_roms);
    get_list("recent_projects", cfg.recent_projects);

    // ---- Input bindings ------------------------------------------------------
    cfg.p2_enabled = tbl["input"]["p2_enabled"].value_or(false);
    auto get_player = [&](const char* player, InputBindings& out) {
        for (int i = 0; i < kGameActions; ++i) {
            const char* key = game_action_key(static_cast<GameAction>(i));
            if (auto v = tbl["input"][player]["keyboard"][key].value<std::string>())
                out.kb[i] = *v;
            if (auto v = tbl["input"][player]["gamepad"][key].value<std::string>())
                out.pad[i] = *v;
        }
    };
    get_player("p1", cfg.input_p1);
    get_player("p2", cfg.input_p2);
    for (int i = 0; i < kLabActions; ++i) {
        const char* key = lab_action_key(static_cast<LabAction>(i));
        // Lee [input.lab]; back-compat: cae a la clave histórica [input.studio].
        if (auto v = tbl["input"]["lab"]["keyboard"][key].value<std::string>())
            cfg.lab_kb[i] = *v;
        else if (auto v = tbl["input"]["studio"]["keyboard"][key].value<std::string>())
            cfg.lab_kb[i] = *v;
        if (auto v = tbl["input"]["lab"]["gamepad"][key].value<std::string>())
            cfg.lab_pad[i] = *v;
        else if (auto v = tbl["input"]["studio"]["gamepad"][key].value<std::string>())
            cfg.lab_pad[i] = *v;
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

void AytherConfig::save() const {
    const std::filesystem::path file = config_file();
    std::ofstream f(file);
    if (!f.is_open()) {
        ayther::log::write(ayther::log::Severity::Error,
            "config", "cannot_write",
            "Cannot write %s",
            file.string().c_str());
        return;
    }

    f << "# Ayther Engine — user configuration\n";
    f << "# Edit freely; the engine rewrites this file on exit.\n\n";

    auto write_str = [&](const char* key, const std::string& val) {
        // Normalise backslashes to forward slashes for readability.
        std::string v = val;
        for (char& c : v) if (c == '\\') c = '/';
        f << key << " = \"" << v << "\"\n";
    };

    write_str("rom_library",  rom_library);
    write_str("packs_dir",    packs_dir);
    write_str("cores_dir",    cores_dir);
    write_str("projects_dir", projects_dir);
    write_str("core_path",    core_path);
    write_str("dat_path",     dat_path);
    write_str("soundfonts_dir", soundfonts_dir);
    write_str("defs_dir",     defs_dir);
    write_str("ra_user",      ra_user);
    write_str("ra_api_key",   ra_api_key);
    write_str("dialog_accept_pad",  dialog_accept_pad);
    f << "pad_type_p1 = " << pad_type[0] << "\n";
    f << "pad_type_p2 = " << pad_type[1] << "\n";
    write_str("dialog_discard_pad", dialog_discard_pad);
    f << "auto_save  = " << (auto_save ? "true" : "false") << "\n";
    f << "mcp_enabled = " << (mcp_enabled ? "true" : "false") << "\n";
    f << "mcp_port = " << mcp_port << "\n";

    auto write_list = [&](const char* key, const std::vector<std::string>& list) {
        f << "\n" << key << " = [\n";
        for (const auto& item : list) {
            std::string v = item;
            for (char& c : v) if (c == '\\') c = '/';
            f << "  \"" << v << "\",\n";
        }
        f << "]\n";
    };
    write_list("recent_roms",     recent_roms);
    write_list("recent_projects", recent_projects);

    // ---- Input bindings (tablas al final: TOML exige tablas tras las claves) -
    f << "\n[input]\n";
    f << "p2_enabled = " << (p2_enabled ? "true" : "false") << "\n";
    auto write_player = [&](const char* player, const InputBindings& b) {
        f << "\n[input." << player << ".keyboard]\n";
        for (int i = 0; i < kGameActions; ++i)
            f << game_action_key(static_cast<GameAction>(i))
              << " = \"" << b.kb[i] << "\"\n";
        f << "\n[input." << player << ".gamepad]\n";
        for (int i = 0; i < kGameActions; ++i)
            f << game_action_key(static_cast<GameAction>(i))
              << " = \"" << b.pad[i] << "\"\n";
    };
    write_player("p1", input_p1);
    write_player("p2", input_p2);
    f << "\n[input.lab.keyboard]\n";
    for (int i = 0; i < kLabActions; ++i)
        f << lab_action_key(static_cast<LabAction>(i))
          << " = \"" << lab_kb[i] << "\"\n";
    f << "\n[input.lab.gamepad]\n";
    for (int i = 0; i < kLabActions; ++i)
        f << lab_action_key(static_cast<LabAction>(i))
          << " = \"" << lab_pad[i] << "\"\n";
}

// ---------------------------------------------------------------------------
// push_recent
// ---------------------------------------------------------------------------

void AytherConfig::push_recent(const std::string& rom_path) {
    // Remove any existing entry with the same path (case-sensitive).
    // write_list normaliza '\'→'/' al GUARDAR: deduplicar con el path CRUDO
    // deja pasar el mismo dir llegado con backslashes (--project en Windows)
    // y cada arranque suma un duplicado exacto tras el save (llegaron a 6
    // cards iguales + conflicto de IDs de ImGui en el launcher, 2026-07-20).
    // Normalizar ANTES de dedupear e insertar ya normalizado.
    std::string rom = rom_path;
    for (char& c : rom) if (c == '\\') c = '/';
    recent_roms.erase(
        std::remove(recent_roms.begin(), recent_roms.end(), rom),
        recent_roms.end());

    // Prepend to front.
    recent_roms.insert(recent_roms.begin(), rom);

    // Cap at kMaxRecent.
    if (static_cast<int>(recent_roms.size()) > kMaxRecent)
        recent_roms.resize(static_cast<size_t>(kMaxRecent));
}

void AytherConfig::push_recent_project(const std::string& project_dir) {
    std::string dir = project_dir;   // mismo criterio que push_recent (ver arriba)
    for (char& c : dir) if (c == '\\') c = '/';
    recent_projects.erase(
        std::remove(recent_projects.begin(), recent_projects.end(), dir),
        recent_projects.end());
    recent_projects.insert(recent_projects.begin(), dir);
    if (static_cast<int>(recent_projects.size()) > kMaxRecent)
        recent_projects.resize(static_cast<size_t>(kMaxRecent));
}
