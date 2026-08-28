//! Sandboxed Lua 5.4 environment for pack scripts.
//!
//! [`ScriptEnv`] exposes a constrained AYTHER API for RAM reads, pack access,
//! substitutions, shader parameters, and per-frame callbacks.

// ---------------------------------------------------------------------------
// ScriptEnv — sandboxed Lua 5.4 runtime for.ay pack scripts.
//
// ## Security sandbox
//
// Only safe standard libraries are loaded:
//   STRING, TABLE, MATH, BIT (Lua bit ops)
// Explicitly excluded: io, os, package, debug, coroutine, utf8.
//
// ## API surface (available to pack scripts)
//
//   ayther.version()                       → current AYTHER release version
//   ayther.log(msg)                        → prints to stdout
//
//   ayther.ram.read_u8(offset)             → u8
//   ayther.ram.read_u16_be(offset)         → u16  big-endian (68000 native)
//   ayther.ram.read_i16_be(offset)         → i16  big-endian (positions, velocity)
//   ayther.ram.read_u32_be(offset)         → u32
//
//   ayther.pack.read(logical_path)         → string (binary) or nil
//   ayther.pack.exists(logical_path)       → bool
//
//   ayther.on_frame(fn)                    → register a per-frame callback
//
// ## Frame model
//
//   1. Host calls ScriptEnv::update_frame(ram, pack)  — refreshes app_data
//   2. Host calls ScriptEnv::call_on_frame()          — fires all registered callbacks
//
// ## CPU budget
//
// A Lua instruction-count hook fires after kMaxInstructionsPerFrame.
// When triggered it raises an error, which propagates to call_on_frame()
// as a Err(LuaError::...) — logged but non-fatal.
// ---------------------------------------------------------------------------

use mlua::{Error as LuaError, Lua, LuaOptions, StdLib, Table, Value};

// ---------------------------------------------------------------------------
// app_data types shared between ScriptEnv and Lua closures
// ---------------------------------------------------------------------------

/// Snapshot of the emulator's work RAM for this frame (cloned, not a pointer).
pub(crate) struct RamSnapshot(pub Vec<u8>);

/// Byte-slice view into the currently-loaded pack.
/// Using raw pointer because AyArchive is behind Box<> and stable.
pub(crate) struct PackPtr(pub *const crate::archive_vfs::AyArchive);

/// Per-frame tile occurrence list — set before on_frame callbacks fire.
pub(crate) struct TileOccurrenceData(pub Vec<crate::sprite_hasher::TileOccurrence>);

/// Per-frame sprite occurrence list — set before on_frame callbacks fire.  (v0.9.3)
pub(crate) struct SpriteOccurrenceData(pub Vec<crate::vram_sprite::SpriteOccurrence>);

/// Per-tick audio occurrence list — set before on_frame callbacks fire.  (v0.9.2)
pub(crate) struct AudioOccurrenceData(pub Vec<crate::audio_hasher::AudioOccurrence>);

// SAFETY: ScriptEnv is single-threaded by design; mlua without "send" feature
// does not require Sync on its app_data values.
unsafe impl Send for PackPtr {}

// ---------------------------------------------------------------------------
// Per-frame budget: max Lua VM instructions before a forced interrupt.
// ---------------------------------------------------------------------------
const MAX_INSTRUCTIONS_PER_FRAME: u32 = 1_000_000;

// ---------------------------------------------------------------------------
// ScriptEnv
// ---------------------------------------------------------------------------

/// Sandboxed Lua environment exposed to pack scripts.
pub struct ScriptEnv {
    lua: Lua,
}

impl ScriptEnv {
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// Creates a sandboxed Lua 5.4 instance and registers the `ayther` global.
    ///
    /// # Errors
    ///
    /// Returns a Lua error if the VM or any built-in API table cannot be
    /// initialized.
    pub fn new() -> Result<Self, LuaError> {
        let lua = Lua::new_with(
            StdLib::STRING | StdLib::TABLE | StdLib::MATH,
            LuaOptions::default(),
        )?;

        // Install instruction-count hook to enforce CPU budget.
        // set_hook returns () in mlua 0.9 (no error).
        lua.set_hook(
            mlua::HookTriggers::new().every_nth_instruction(MAX_INSTRUCTIONS_PER_FRAME),
            |_lua, _debug| {
                Err(LuaError::RuntimeError(
                    "ayther: script exceeded per-frame CPU budget".into(),
                ))
            },
        );

        let env = ScriptEnv { lua };
        env.register_ayther_table()?;
        Ok(env)
    }

    // -----------------------------------------------------------------------
    // Frame update
    // -----------------------------------------------------------------------

    /// Snapshot the emulator RAM so Lua can read it this frame.
    pub fn update_ram(&self, ram: &[u8]) {
        self.lua.set_app_data(RamSnapshot(ram.to_vec()));
    }

    /// Point the script environment at the loaded.ay pack (null = no pack).
    pub fn set_pack(&self, pack: *const crate::archive_vfs::AyArchive) {
        self.lua.set_app_data(PackPtr(pack));
    }

    /// Push the current frame's tile occurrences so `ayther.tiles.list()` works
    /// inside on_frame callbacks.  Call this BEFORE call_on_frame().
    pub fn update_tiles(&self, occurrences: &[crate::sprite_hasher::TileOccurrence]) {
        self.lua
            .set_app_data(TileOccurrenceData(occurrences.to_vec()));
    }

    /// Push the current frame's sprite occurrences so `ayther.sprites.list()` works
    /// inside on_frame callbacks.  Call this BEFORE call_on_frame().  (v0.9.3)
    pub fn update_sprites(&self, occurrences: &[crate::vram_sprite::SpriteOccurrence]) {
        self.lua
            .set_app_data(SpriteOccurrenceData(occurrences.to_vec()));
    }

    /// Read sprite override entries registered by `ayther.sprites.replace()` during
    /// the last on_frame execution.  Returns `(hash_u64, asset_path)` pairs.  (v0.9.3)
    pub fn get_sprite_overrides(&self) -> Vec<(u64, String)> {
        let get = || -> mlua::Result<Vec<(u64, String)>> {
            let ayther: Table = self.lua.globals().get("ayther")?;
            let overrides: Table = ayther.get("_sprite_overrides")?;
            let mut result = Vec::new();
            for pair in overrides.pairs::<String, String>() {
                let (k, v) = pair?;
                let hex = k
                    .trim()
                    .strip_prefix("0x")
                    .or_else(|| k.trim().strip_prefix("0X"))
                    .unwrap_or(k.trim());
                if let Ok(hash) = u64::from_str_radix(hex, 16) {
                    result.push((hash, v));
                }
            }
            Ok(result)
        };
        match get() {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[ScriptEnv] get_sprite_overrides: {}", e);
                vec![]
            }
        }
    }

    /// Push the current tick's audio occurrences so `ayther.audio.list()` works
    /// inside on_frame callbacks.  Call this BEFORE call_on_frame().  (v0.9.2)
    pub fn update_audio(&self, occurrences: &[crate::audio_hasher::AudioOccurrence]) {
        self.lua
            .set_app_data(AudioOccurrenceData(occurrences.to_vec()));
    }

    /// Read tile override entries registered by `ayther.tiles.replace()` during
    /// the last on_frame execution.  Returns `(hash_u64, asset_path)` pairs.
    pub fn get_tile_overrides(&self) -> Vec<(u64, String)> {
        let get = || -> mlua::Result<Vec<(u64, String)>> {
            let ayther: Table = self.lua.globals().get("ayther")?;
            let overrides: Table = ayther.get("_tile_overrides")?;
            let mut result = Vec::new();
            for pair in overrides.pairs::<String, String>() {
                let (k, v) = pair?;
                let hex = k
                    .trim()
                    .strip_prefix("0x")
                    .or_else(|| k.trim().strip_prefix("0X"))
                    .unwrap_or(k.trim());
                if let Ok(hash) = u64::from_str_radix(hex, 16) {
                    result.push((hash, v));
                }
            }
            Ok(result)
        };
        match get() {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[ScriptEnv] get_tile_overrides: {}", e);
                vec![]
            }
        }
    }

    /// Read the shader parameters set by `ayther.shader.set_param()`.
    /// Returns `(crt_strength, scan_strength, vignette)` in [0, 1].  (v0.9.4)
    pub fn get_shader_params(&self) -> (f32, f32, f32) {
        let get = || -> mlua::Result<(f32, f32, f32)> {
            let ayther: Table = self.lua.globals().get("ayther")?;
            let shader: Table = ayther.get("shader")?;
            let params: Table = shader.get("_params")?;
            let crt: f32 = params.get("crt_strength").unwrap_or(0.0_f32);
            let scan: f32 = params.get("scan_strength").unwrap_or(0.5_f32);
            let vig: f32 = params.get("vignette").unwrap_or(0.2_f32);
            Ok((crt, scan, vig))
        };
        match get() {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[ScriptEnv] get_shader_params: {}", e);
                (0.0, 0.5, 0.2)
            }
        }
    }

    /// Returns the native-pixel safe area selected by the script this frame.
    ///
    /// `None` means the script did not override the full native frame.
    pub fn get_safe_area(&self) -> Option<(i32, i32)> {
        let get = || -> mlua::Result<Option<(i32, i32)>> {
            let ayther: Table = self.lua.globals().get("ayther")?;
            let t: Table = ayther.get("_safe_area")?;
            let x0: Option<i64> = t.get("x0").ok();
            let x1: Option<i64> = t.get("x1").ok();
            Ok(match (x0, x1) {
                (Some(a), Some(b)) if b > a => Some((a as i32, b as i32)),
                _ => None,
            })
        };
        match get() {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[ScriptEnv] get_safe_area: {}", e);
                None
            }
        }
    }

    /// Read audio override entries registered by `ayther.audio.replace()` during
    /// the last on_frame execution.  Returns `(hash_u64, asset_path)` pairs.  (v0.9.2)
    pub fn get_audio_overrides(&self) -> Vec<(u64, String)> {
        let get = || -> mlua::Result<Vec<(u64, String)>> {
            let ayther: Table = self.lua.globals().get("ayther")?;
            let overrides: Table = ayther.get("_audio_overrides")?;
            let mut result = Vec::new();
            for pair in overrides.pairs::<String, String>() {
                let (k, v) = pair?;
                let hex = k
                    .trim()
                    .strip_prefix("0x")
                    .or_else(|| k.trim().strip_prefix("0X"))
                    .unwrap_or(k.trim());
                if let Ok(hash) = u64::from_str_radix(hex, 16) {
                    result.push((hash, v));
                }
            }
            Ok(result)
        };
        match get() {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[ScriptEnv] get_audio_overrides: {}", e);
                vec![]
            }
        }
    }

    /// Load and execute a Lua source string.
    ///
    /// `chunk_name` is used in error messages (e.g. "scripts/init.lua").
    ///
    /// # Errors
    ///
    /// Returns a Lua compile-time or runtime error from the chunk.
    pub fn load_string(&self, source: &str, chunk_name: &str) -> Result<(), LuaError> {
        self.lua.load(source).set_name(chunk_name).exec()
    }

    /// Fire all callbacks registered with `ayther.on_frame(fn)`.
    ///
    /// Callbacks are stored in `ayther._callbacks` (a Lua-side array).
    /// Returns the number of callbacks that completed without error.
    /// Per-callback errors are printed to stderr; execution continues.
    /// Returns the number of callbacks registered with `ayther.on_frame`.
    pub fn on_frame_count(&self) -> u32 {
        (|| -> mlua::Result<u32> {
            let ayther: Table = self.lua.globals().get("ayther")?;
            let cbs: Table = ayther.get("_callbacks")?;
            Ok(cbs.raw_len() as u32)
        })()
        .unwrap_or(0)
    }

    /// Invokes all registered frame callbacks and returns the successful count.
    ///
    /// Individual callback failures are reported to standard error without
    /// preventing later callbacks from running.
    pub fn call_on_frame(&self) -> u32 {
        let callbacks_result: mlua::Result<Table> = (|| {
            let ayther: Table = self.lua.globals().get("ayther")?;
            ayther.get("_callbacks")
        })();

        let callbacks = match callbacks_result {
            Ok(t) => t,
            Err(e) => {
                eprintln!("[ScriptEnv] _callbacks access error: {}", e);
                return 0;
            }
        };

        let mut ok = 0u32;
        let n = callbacks.raw_len();
        for i in 1..=n {
            let cb: mlua::Function = match callbacks.raw_get(i) {
                Ok(f) => f,
                Err(e) => {
                    eprintln!("[ScriptEnv] callback[{}] not a function: {}", i, e);
                    continue;
                }
            };
            match cb.call::<_, ()>(()) {
                Ok(_) => ok += 1,
                Err(e) => eprintln!("[ScriptEnv] on_frame[{}] error: {}", i, e),
            }
        }
        ok
    }

    // Legacy alias used by v0.1.0 stub callers.
    /// Invokes frame callbacks and discards the successful-callback count.
    pub fn call_on_tick(&self) {
        let _ = self.call_on_frame();
    }

    // -----------------------------------------------------------------------
    // Register ayther.* global table
    // -----------------------------------------------------------------------

    fn register_ayther_table(&self) -> Result<(), LuaError> {
        let lua = &self.lua;
        let globals = lua.globals();

        let ayther: Table = lua.create_table()?;

        // ---- ayther.version() ----------------------------------------------
        ayther.set(
            "version",
            lua.create_function(|_, ()| Ok(crate::RELEASE_VERSION))?,
        )?;

        // ---- ayther.log(msg) -----------------------------------------------
        ayther.set(
            "log",
            lua.create_function(|_, msg: String| {
                println!("[Lua] {}", msg);
                Ok(())
            })?,
        )?;

        // ---- ayther.on_frame(fn) -------------------------------------------
        // We can't push to frame_callbacks from inside a closure that doesn't
        // own ScriptEnv, so we use a shared Vec via app_data.
        // Instead, scripts call `ayther.on_frame(fn)` which stores the
        // function in a Lua-side list `ayther._callbacks[]`, and
        // call_on_frame() reads that list each tick.
        ayther.set("_callbacks", lua.create_table()?)?;
        ayther.set(
            "on_frame",
            lua.create_function(|lua_ctx, cb: mlua::Function| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                let callbacks: Table = ayther_tbl.get("_callbacks")?;
                let n = callbacks.raw_len();
                callbacks.raw_set(n + 1, cb)?;
                Ok(())
            })?,
        )?;

        // ---- ayther.ram.* --------------------------------------------------
        let ram_tbl: Table = lua.create_table()?;

        ram_tbl.set(
            "read_u8",
            lua.create_function(|lua_ctx, offset: usize| {
                if let Some(snap) = lua_ctx.app_data_ref::<RamSnapshot>()
                    && offset < snap.0.len()
                {
                    return Ok(snap.0[offset] as u32);
                }
                Ok(0u32)
            })?,
        )?;

        ram_tbl.set(
            "read_u16_be",
            lua.create_function(|lua_ctx, offset: usize| {
                if let Some(snap) = lua_ctx.app_data_ref::<RamSnapshot>() {
                    let r = &snap.0;
                    if offset + 1 < r.len() {
                        return Ok(u16::from_be_bytes([r[offset], r[offset + 1]]) as u32);
                    }
                }
                Ok(0u32)
            })?,
        )?;

        ram_tbl.set(
            "read_i16_be",
            lua.create_function(|lua_ctx, offset: usize| {
                if let Some(snap) = lua_ctx.app_data_ref::<RamSnapshot>() {
                    let r = &snap.0;
                    if offset + 1 < r.len() {
                        return Ok(i16::from_be_bytes([r[offset], r[offset + 1]]) as i32);
                    }
                }
                Ok(0i32)
            })?,
        )?;

        ram_tbl.set(
            "read_u32_be",
            lua.create_function(|lua_ctx, offset: usize| {
                if let Some(snap) = lua_ctx.app_data_ref::<RamSnapshot>() {
                    let r = &snap.0;
                    if offset + 3 < r.len() {
                        return Ok(u32::from_be_bytes([
                            r[offset],
                            r[offset + 1],
                            r[offset + 2],
                            r[offset + 3],
                        ]));
                    }
                }
                Ok(0u32)
            })?,
        )?;

        ayther.set("ram", ram_tbl)?;

        // ---- ayther.pack.* -------------------------------------------------
        let pack_tbl: Table = lua.create_table()?;

        pack_tbl.set(
            "read",
            lua.create_function(|lua_ctx, path: String| {
                if let Some(pp) = lua_ctx.app_data_ref::<PackPtr>()
                    && !pp.0.is_null()
                {
                    // SAFETY: pointer is valid while pack is alive (same frame).
                    if let Some(data) = unsafe { (*pp.0).read(&path) } {
                        return Ok(Value::String(lua_ctx.create_string(data)?));
                    }
                }
                Ok(Value::Nil)
            })?,
        )?;

        pack_tbl.set(
            "exists",
            lua.create_function(|lua_ctx, path: String| {
                if let Some(pp) = lua_ctx.app_data_ref::<PackPtr>()
                    && !pp.0.is_null()
                {
                    // SAFETY: The pack pointer remains valid while it is attached
                    // to this Lua context.
                    return Ok(unsafe { (*pp.0).file_size(&path) }.is_some());
                }
                Ok(false)
            })?,
        )?;

        ayther.set("pack", pack_tbl)?;

        // ---- ayther.tiles.* ------------------------------------------------
        // _tile_overrides: hash_hex → asset_path  (populated by tiles.replace)
        ayther.set("_tile_overrides", lua.create_table()?)?;

        let tiles_tbl: Table = lua.create_table()?;

        // ayther.tiles.list() → array of {hash, tile_x, tile_y}
        tiles_tbl.set(
            "list",
            lua.create_function(|lua_ctx, ()| {
                let result: Table = lua_ctx.create_table()?;
                if let Some(data) = lua_ctx.app_data_ref::<TileOccurrenceData>() {
                    for (i, occ) in data.0.iter().enumerate() {
                        let t: Table = lua_ctx.create_table()?;
                        t.set("hash", format!("{:#018x}", occ.hash))?;
                        t.set("tile_x", occ.tile_x)?;
                        t.set("tile_y", occ.tile_y)?;
                        result.raw_set(i as i64 + 1, t)?;
                    }
                }
                Ok(result)
            })?,
        )?;

        // ayther.tiles.replace(hash_hex, asset_path) — register an override
        tiles_tbl.set(
            "replace",
            lua.create_function(|lua_ctx, (hash_hex, asset_path): (String, String)| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                let overrides: Table = ayther_tbl.get("_tile_overrides")?;
                overrides.set(hash_hex, asset_path)?;
                Ok(())
            })?,
        )?;

        // ayther.tiles.clear() — remove all script overrides for this frame
        tiles_tbl.set(
            "clear",
            lua.create_function(|lua_ctx, ()| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                // Replace with a fresh empty table (simplest GC-friendly approach).
                ayther_tbl.set("_tile_overrides", lua_ctx.create_table()?)?;
                Ok(())
            })?,
        )?;

        // ---- ayther.screen.* ---------------------------------
        // El AREA SEGURA por frame. El area donde el HUD sigue siendo valido no
        // es una constante: en un beat 'em up la camara SE FIJA durante la
        // pelea y el personaje recorre toda la pantalla, asi que un area fija
        // seria incorrecta en los dos extremos del recorrido.
        //
        // La REGLA vive aca y no en el motor: cada juego resuelve su camara
        // distinto, y cablear una heuristica en C++ seria adivinar por todos.
        // El DATO lo aporta Modo 3 (la posicion sale de la RAM por las anclas
        // del perfil) y el script decide como el area lo sigue.
        ayther.set("_safe_area", lua.create_table()?)?;

        let screen_tbl: Table = lua.create_table()?;

        // ayther.screen.safe_area(x0, x1) — fija el area segura de ESTE frame,
        // en pixeles del espacio nativo. Sin llamada, el area es la de siempre
        // (el frame nativo entero), que es el comportamiento previo.
        screen_tbl.set(
            "safe_area",
            lua.create_function(|lua_ctx, (x0, x1): (i64, i64)| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                let t: Table = ayther_tbl.get("_safe_area")?;
                // Invertido o vacio = sin area: es distinto de «area de ancho 0»,
                // que dejaria TODO afuera. Ante la duda, no se filtra nada.
                if x1 > x0 {
                    t.set("x0", x0)?;
                    t.set("x1", x1)?;
                }
                Ok(())
            })?,
        )?;

        // ayther.screen.clear_safe_area() — vuelve al area de siempre.
        screen_tbl.set(
            "clear_safe_area",
            lua.create_function(|lua_ctx, ()| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                ayther_tbl.set("_safe_area", lua_ctx.create_table()?)?;
                Ok(())
            })?,
        )?;

        ayther.set("screen", screen_tbl)?;

        ayther.set("tiles", tiles_tbl)?;

        // ---- ayther.sprites.* ----------------------------------------------
        // _sprite_overrides: hash_hex → asset_path  (populated by sprites.replace)
        ayther.set("_sprite_overrides", lua.create_table()?)?;

        let sprites_tbl: Table = lua.create_table()?;

        // ayther.sprites.list() → array of {hash, w_tiles, h_tiles, screen_x, screen_y}
        sprites_tbl.set(
            "list",
            lua.create_function(|lua_ctx, ()| {
                let result: Table = lua_ctx.create_table()?;
                if let Some(data) = lua_ctx.app_data_ref::<SpriteOccurrenceData>() {
                    for (i, occ) in data.0.iter().enumerate() {
                        let t: Table = lua_ctx.create_table()?;
                        t.set("hash", format!("{:#018x}", occ.hash))?;
                        t.set("w_tiles", occ.w_tiles as u32)?;
                        t.set("h_tiles", occ.h_tiles as u32)?;
                        t.set("screen_x", occ.screen_x as i32)?;
                        t.set("screen_y", occ.screen_y as i32)?;
                        result.raw_set(i as i64 + 1, t)?;
                    }
                }
                Ok(result)
            })?,
        )?;

        // ayther.sprites.replace(hash_hex, asset_path) — register a runtime override.
        // Example: ayther.sprites.replace("0xdeadbeef12345678", "sprites/sonic/run.png")
        sprites_tbl.set(
            "replace",
            lua.create_function(|lua_ctx, (hash_hex, asset_path): (String, String)| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                let overrides: Table = ayther_tbl.get("_sprite_overrides")?;
                overrides.set(hash_hex, asset_path)?;
                Ok(())
            })?,
        )?;

        // ayther.sprites.clear() — remove all script overrides for this frame.
        sprites_tbl.set(
            "clear",
            lua.create_function(|lua_ctx, ()| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                ayther_tbl.set("_sprite_overrides", lua_ctx.create_table()?)?;
                Ok(())
            })?,
        )?;

        ayther.set("sprites", sprites_tbl)?;

        // ---- ayther.audio.* ------------------------------------------------
        // _audio_overrides: hash_hex → asset_path  (populated by audio.replace)
        ayther.set("_audio_overrides", lua.create_table()?)?;

        let audio_tbl: Table = lua.create_table()?;

        // ayther.audio.list() → array of {hash, frame_count, hits}
        audio_tbl.set(
            "list",
            lua.create_function(|lua_ctx, ()| {
                let result: Table = lua_ctx.create_table()?;
                if let Some(data) = lua_ctx.app_data_ref::<AudioOccurrenceData>() {
                    for (i, occ) in data.0.iter().enumerate() {
                        let t: Table = lua_ctx.create_table()?;
                        t.set("hash", format!("{:#018x}", occ.hash))?;
                        t.set("frame_count", occ.frame_count as u32)?;
                        t.set("hits", occ.hits)?;
                        result.raw_set(i as i64 + 1, t)?;
                    }
                }
                Ok(result)
            })?,
        )?;

        // ayther.audio.replace(hash_hex, asset_path) — register a runtime override.
        // Example: ayther.audio.replace("0xdeadbeef12345678", "audio/sfx/ring.wav")
        audio_tbl.set(
            "replace",
            lua.create_function(|lua_ctx, (hash_hex, asset_path): (String, String)| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                let overrides: Table = ayther_tbl.get("_audio_overrides")?;
                overrides.set(hash_hex, asset_path)?;
                Ok(())
            })?,
        )?;

        // ayther.audio.clear() — remove all script overrides for this frame.
        audio_tbl.set(
            "clear",
            lua.create_function(|lua_ctx, ()| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                ayther_tbl.set("_audio_overrides", lua_ctx.create_table()?)?;
                Ok(())
            })?,
        )?;

        ayther.set("audio", audio_tbl)?;

        // ---- ayther.shader.* -----------------------------------------------
        // _params: { crt_strength, scan_strength, vignette }
        // Defaults: crt_strength=0.0 (off), scan_strength=0.5, vignette=0.2
        //
        // Usage:
        //   ayther.shader.set_param("crt_strength",  0.8)
        //   ayther.shader.set_param("scan_strength", 0.6)
        //   ayther.shader.set_param("vignette",      0.3)
        let shader_tbl: Table = lua.create_table()?;

        let shader_params: Table = lua.create_table()?;
        shader_params.set("crt_strength", 0.0_f32)?;
        shader_params.set("scan_strength", 0.5_f32)?;
        shader_params.set("vignette", 0.2_f32)?;
        shader_tbl.set("_params", shader_params)?;

        // ayther.shader.set_param(name, value)
        // Recognized names: "crt_strength", "scan_strength", "vignette".
        // Unknown names are silently ignored (forward-compat for future params).
        shader_tbl.set(
            "set_param",
            lua.create_function(|lua_ctx, (name, value): (String, f32)| {
                const KNOWN: &[&str] = &["crt_strength", "scan_strength", "vignette"];
                if KNOWN.contains(&name.as_str()) {
                    let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                    let shader: Table = ayther_tbl.get("shader")?;
                    let params: Table = shader.get("_params")?;
                    params.set(name, value)?;
                }
                Ok(())
            })?,
        )?;

        // ayther.shader.get_param(name) → number
        shader_tbl.set(
            "get_param",
            lua.create_function(|lua_ctx, name: String| {
                let ayther_tbl: Table = lua_ctx.globals().get("ayther")?;
                let shader: Table = ayther_tbl.get("shader")?;
                let params: Table = shader.get("_params")?;
                let val: mlua::Value = params.get(name)?;
                match val {
                    mlua::Value::Number(n) => Ok(n as f32),
                    mlua::Value::Integer(i) => Ok(i as f32),
                    _ => Ok(0.0_f32),
                }
            })?,
        )?;

        ayther.set("shader", shader_tbl)?;

        globals.set("ayther", ayther)?;
        Ok(())
    }
}

impl Default for ScriptEnv {
    fn default() -> Self {
        Self::new().expect("ScriptEnv::new() failed")
    }
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn make_ram(size: usize) -> Vec<u8> {
        let mut v = vec![0u8; size];
        // Write a known value at offset 0x10 (big-endian i16 = 1234)
        let be = (1234i16).to_be_bytes();
        v[0x10] = be[0];
        v[0x11] = be[1];
        v
    }

    #[test]
    fn creates_without_error() {
        ScriptEnv::new().expect("ScriptEnv::new()");
    }

    #[test]
    fn version_returns_string() {
        let env = ScriptEnv::new().unwrap();
        let version: String = env.lua.load("return ayther.version()").eval().unwrap();
        assert_eq!(version, crate::RELEASE_VERSION);
    }

    #[test]
    fn ram_read_i16_be() {
        let env = ScriptEnv::new().unwrap();
        env.update_ram(&make_ram(0x100));
        env.lua
            .load("assert(ayther.ram.read_i16_be(0x10) == 1234, 'wrong value')")
            .exec()
            .unwrap();
    }

    #[test]
    fn ram_out_of_bounds_returns_zero() {
        let env = ScriptEnv::new().unwrap();
        env.update_ram(&[0xFFu8; 4]);
        env.lua
            .load("assert(ayther.ram.read_u8(9999) == 0)")
            .exec()
            .unwrap();
    }

    #[test]
    fn on_frame_callback_fires() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            _counter = 0
            ayther.on_frame(function()
                _counter = _counter + 1
            end)
        "#,
            "test",
        )
        .unwrap();

        env.update_ram(&[0u8; 64]);
        let fired = env.call_on_frame();
        assert_eq!(fired, 1);

        // Counter should be 1 after one tick
        let counter: i64 = env.lua.globals().get("_counter").unwrap();
        assert_eq!(counter, 1);
    }

    #[test]
    fn cpu_budget_is_enforced() {
        let env = ScriptEnv::new().unwrap();
        // Infinite loop — should be interrupted by the instruction hook.
        env.load_string(
            r#"
            ayther.on_frame(function()
                while true do end  -- infinite loop
            end)
        "#,
            "infinite_loop",
        )
        .unwrap();

        env.update_ram(&[0u8; 64]);
        // Should not hang — the hook interrupts after MAX_INSTRUCTIONS_PER_FRAME.
        let _ = env.call_on_frame(); // returns 0 (error swallowed)
    }

    #[test]
    fn pack_exists_returns_false_when_no_pack() {
        let env = ScriptEnv::new().unwrap();
        env.set_pack(std::ptr::null());
        env.lua
            .load("assert(ayther.pack.exists('graphics/tile.png') == false)")
            .exec()
            .unwrap();
    }

    #[test]
    fn log_does_not_crash() {
        let env = ScriptEnv::new().unwrap();
        env.load_string("ayther.log('hello from Lua')", "log_test")
            .unwrap();
    }

    #[test]
    fn tiles_list_returns_occurrences() {
        use crate::sprite_hasher::TileOccurrence;
        let env = ScriptEnv::new().unwrap();
        env.update_tiles(&[
            TileOccurrence {
                hash: 0xdeadbeef,
                tile_x: 2,
                tile_y: 5,
            },
            TileOccurrence {
                hash: 0xcafe1234,
                tile_x: 7,
                tile_y: 0,
            },
        ]);
        env.lua
            .load(
                r#"
            local tiles = ayther.tiles.list()
            assert(#tiles == 2, 'expected 2 tiles, got ' .. #tiles)
            assert(tiles[1].tile_x == 2)
            assert(tiles[2].tile_x == 7)
        "#,
            )
            .exec()
            .unwrap();
    }

    #[test]
    fn tiles_replace_and_get_overrides() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.tiles.replace("0xdeadbeef", "hd/tile.png")
        "#,
            "test",
        )
        .unwrap();
        let overrides = env.get_tile_overrides();
        assert_eq!(overrides.len(), 1);
        assert_eq!(overrides[0].0, 0xdeadbeef);
        assert_eq!(overrides[0].1, "hd/tile.png");
    }

    #[test]
    fn tiles_clear_removes_overrides() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.tiles.replace("0xdeadbeef", "hd/tile.png")
            ayther.tiles.clear()
        "#,
            "test",
        )
        .unwrap();
        assert!(env.get_tile_overrides().is_empty());
    }

    // ---- Sprite API tests (v0.9.3) -----------------------------------------

    #[test]
    fn sprites_list_returns_occurrences() {
        use crate::vram_sprite::SpriteOccurrence;
        let env = ScriptEnv::new().unwrap();
        env.update_sprites(&[
            SpriteOccurrence {
                hash: 0x1122334455667788,
                anim_group_id: 0,
                w_tiles: 2,
                h_tiles: 4,
                screen_x: 64,
                screen_y: 128,
                link: 0,
                palette: 0,
                priority: 0,
                slot: 0,
                hflip: 0,
                vflip: 0,
            },
            SpriteOccurrence {
                hash: 0xaabbccddeeff0011,
                anim_group_id: 0,
                w_tiles: 1,
                h_tiles: 2,
                screen_x: -8,
                screen_y: 200,
                link: 0,
                palette: 1,
                priority: 1,
                slot: 0,
                hflip: 0,
                vflip: 0,
            },
        ]);
        env.lua
            .load(
                r#"
            local sp = ayther.sprites.list()
            assert(#sp == 2, 'expected 2 sprites, got ' .. #sp)
            assert(sp[1].w_tiles  == 2)
            assert(sp[1].h_tiles  == 4)
            assert(sp[1].screen_x == 64)
            assert(sp[2].screen_x == -8)
        "#,
            )
            .exec()
            .unwrap();
    }

    #[test]
    fn sprites_replace_and_get_overrides() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.sprites.replace("0x1122334455667788", "sprites/sonic/run.png")
        "#,
            "test",
        )
        .unwrap();
        let overrides = env.get_sprite_overrides();
        assert_eq!(overrides.len(), 1);
        assert_eq!(overrides[0].0, 0x1122334455667788);
        assert_eq!(overrides[0].1, "sprites/sonic/run.png");
    }

    #[test]
    fn sprites_clear_removes_overrides() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.sprites.replace("0x1122334455667788", "sprites/sonic/run.png")
            ayther.sprites.clear()
        "#,
            "test",
        )
        .unwrap();
        assert!(env.get_sprite_overrides().is_empty());
    }

    // ---- Audio API tests (v0.9.2) ------------------------------------------

    #[test]
    fn audio_list_returns_occurrences() {
        use crate::audio_hasher::AudioOccurrence;
        let env = ScriptEnv::new().unwrap();
        env.update_audio(&[
            AudioOccurrence {
                hash: 0xaabbccdd11223344,
                frame_count: 735,
                hits: 3,
            },
            AudioOccurrence {
                hash: 0x0102030405060708,
                frame_count: 100,
                hits: 1,
            },
        ]);
        env.lua
            .load(
                r#"
            local sounds = ayther.audio.list()
            assert(#sounds == 2, 'expected 2 sounds, got ' .. #sounds)
            assert(sounds[1].frame_count == 735)
            assert(sounds[1].hits       == 3)
            assert(sounds[2].hits       == 1)
        "#,
            )
            .exec()
            .unwrap();
    }

    #[test]
    fn audio_replace_and_get_overrides() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.audio.replace("0xaabbccdd11223344", "audio/sfx/ring.wav")
        "#,
            "test",
        )
        .unwrap();
        let overrides = env.get_audio_overrides();
        assert_eq!(overrides.len(), 1);
        assert_eq!(overrides[0].0, 0xaabbccdd11223344);
        assert_eq!(overrides[0].1, "audio/sfx/ring.wav");
    }

    #[test]
    fn audio_clear_removes_overrides() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.audio.replace("0xaabbccdd11223344", "audio/sfx/ring.wav")
            ayther.audio.clear()
        "#,
            "test",
        )
        .unwrap();
        assert!(env.get_audio_overrides().is_empty());
    }

    // ---- Shader API tests (v0.9.4) -----------------------------------------

    #[test]
    fn shader_default_params() {
        let env = ScriptEnv::new().unwrap();
        let (crt, scan, vig) = env.get_shader_params();
        assert!((crt - 0.0_f32).abs() < 1e-6, "crt default should be 0.0");
        assert!((scan - 0.5_f32).abs() < 1e-6, "scan default should be 0.5");
        assert!(
            (vig - 0.2_f32).abs() < 1e-6,
            "vignette default should be 0.2"
        );
    }

    #[test]
    fn shader_set_and_get_params() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.shader.set_param("crt_strength",  0.8)
            ayther.shader.set_param("scan_strength", 0.6)
            ayther.shader.set_param("vignette",      0.3)
        "#,
            "test",
        )
        .unwrap();
        let (crt, scan, vig) = env.get_shader_params();
        assert!((crt - 0.8_f32).abs() < 1e-5, "crt should be 0.8, got {crt}");
        assert!(
            (scan - 0.6_f32).abs() < 1e-5,
            "scan should be 0.6, got {scan}"
        );
        assert!(
            (vig - 0.3_f32).abs() < 1e-5,
            "vignette should be 0.3, got {vig}"
        );
    }

    #[test]
    fn shader_unknown_param_ignored() {
        let env = ScriptEnv::new().unwrap();
        // Setting an unknown param must not crash or alter the known defaults.
        env.load_string(
            r#"
            ayther.shader.set_param("nonexistent_param", 99.0)
        "#,
            "test",
        )
        .unwrap();
        let (crt, scan, vig) = env.get_shader_params();
        assert!((crt - 0.0_f32).abs() < 1e-6);
        assert!((scan - 0.5_f32).abs() < 1e-6);
        assert!((vig - 0.2_f32).abs() < 1e-6);
    }

    #[test]
    fn shader_get_param_lua_roundtrip() {
        let env = ScriptEnv::new().unwrap();
        env.load_string(
            r#"
            ayther.shader.set_param("crt_strength", 0.75)
            assert(math.abs(ayther.shader.get_param("crt_strength") - 0.75) < 0.001)
        "#,
            "test",
        )
        .unwrap();
    }
}

#[cfg(test)]
mod safe_area_tests {
    use super::*;

    fn env() -> ScriptEnv {
        ScriptEnv::new().expect("lua")
    }

    #[test]
    fn no_call_means_no_safe_area() {
        let e = env();
        e.load_string("ayther.on_frame(function() end)", "t")
            .unwrap();
        e.call_on_frame();
        // None = «vale la de siempre», que es distinto de un area vacia: un
        // area de ancho 0 dejaria TODO el HUD afuera.
        assert_eq!(e.get_safe_area(), None);
    }

    #[test]
    fn script_sets_safe_area() {
        let e = env();
        e.load_string(
            "ayther.on_frame(function() ayther.screen.safe_area(39, 359) end)",
            "t",
        )
        .unwrap();
        e.call_on_frame();
        assert_eq!(e.get_safe_area(), Some((39, 359)));
    }

    #[test]
    fn inverted_area_is_ignored() {
        let e = env();
        e.load_string(
            "ayther.on_frame(function() ayther.screen.safe_area(300, 100) end)",
            "t",
        )
        .unwrap();
        e.call_on_frame();
        // Ante un rango imposible NO se filtra nada: es mejor no re-anclar que
        // re-anclar contra un area que no existe.
        assert_eq!(e.get_safe_area(), None);
    }

    #[test]
    fn safe_area_can_be_cleared() {
        let e = env();
        e.load_string(
            "ayther.on_frame(function() ayther.screen.safe_area(10, 20) end)",
            "t",
        )
        .unwrap();
        e.call_on_frame();
        assert!(e.get_safe_area().is_some());
        e.load_string(
            "ayther.on_frame(function() ayther.screen.clear_safe_area() end)",
            "t",
        )
        .unwrap();
        e.call_on_frame();
        assert_eq!(e.get_safe_area(), None);
    }

    /// El caso que motiva la feature: en un beat 'em up la camara se fija y el
    /// personaje recorre la pantalla, asi que el area SIGUE su posicion.
    #[test]
    fn safe_area_can_follow_character() {
        let e = env();
        e.load_string(
            "px = 100\n\
             ayther.on_frame(function()\n\
               ayther.screen.safe_area(px - 40, px + 40)\n\
               px = px + 50\n\
             end)",
            "t",
        )
        .unwrap();
        e.call_on_frame();
        assert_eq!(e.get_safe_area(), Some((60, 140)));
        e.call_on_frame();
        assert_eq!(e.get_safe_area(), Some((110, 190)));
    }
}
