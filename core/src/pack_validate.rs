//! Compatibility and authorship validation for `.ay` packs.
//!
//! Validation produces stable, structured findings for both human-readable
//! reports and tool-consumable JSON without executing pack scripts.

// ---------------------------------------------------------------------------
// pack_validate.rs — ¿este pack puede correr con ESTA sesión?
//
// La validación PREVIA: se contesta antes de iniciar, sin ejecutar nada, y
// devuelve una lista de hallazgos en vez de un booleano.
//
// POR QUÉ UNA LISTA Y NO UN SÍ/NO. Porque hay dos cosas distintas que un pack
// puede tener mal y tratarlas igual arruina las dos:
//
//   · una INCOMPATIBILIDAD CRÍTICA —el pack es de otro juego, o pide un Engine
//     que no existe todavía— donde abrir igual serviría contenido equivocado;
//   · una DEGRADACIÓN OPCIONAL —el pack trae un subsistema que este build no
//     conoce, o se horneó con otro core— donde lo correcto es correr y avisar.
//
// Con un booleano, o se rechaza lo segundo (y un pack perfectamente usable no
// abre) o se acepta lo primero (y el usuario ve el pack de otro juego encima
// del suyo, preguntándose qué rompió).
//
// NO ABRE EL PACK CON `AyArchive::open`, a propósito. Ese camino RECHAZA lo que
// no entiende —un esquema más nuevo, una firma que no verifica— y devuelve
// `None`: perfecto para cargar, inútil para diagnosticar. Acá se lee el ZIP y
// el manifest a mano, tolerando todo, porque el trabajo de esta función es
// EXPLICAR por qué algo no va a andar. Es lo que hace que un pack incompatible
// no pueda cerrar la sesión: nunca se llega a abrirlo.
// ---------------------------------------------------------------------------

use crate::archive_vfs::{MANIFEST_SCHEMA, PACK_FORMAT};
use crate::pack_security::{
    EntryMetadata, MAX_ENTRY_COUNT, validate_archive_metadata, validate_archive_size,
    validate_canonical_logical_path,
};
use serde::Deserialize;
use std::collections::HashSet;
use std::io::Read;

/// AYTHER release version compared with pack `ayther_min`.
///
/// This alias preserves the validation API while sourcing the value from the
/// canonical Cargo package version.
pub const ENGINE_VERSION: &str = crate::RELEASE_VERSION;

/// Operational severity of a validation finding.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    /// The pack cannot be used in this session.
    Error,
    /// The pack remains usable with reduced functionality.
    Warning,
    /// A non-blocking authoring recommendation.
    Info,
}

/// One validation finding with a stable machine code and human-readable text.
#[derive(Debug, Clone)]
pub struct Finding {
    /// Operational impact of the finding.
    pub severity: Severity,
    /// Stable identifier intended for tools and user interfaces.
    pub code: &'static str,
    /// Human-readable explanation.
    pub message: String,
}

/// Runtime session facts against which a pack is validated.
///
/// Unknown values remain `None`; the report identifies checks that could not be
/// performed instead of silently accepting them.
#[derive(Debug, Default, Clone)]
pub struct SessionCtx<'a> {
    /// CRC-32 of the loaded ROM.
    pub rom_crc32: Option<u32>,
    /// Session platform, such as `"megadrive"` or `"segacd"`.
    pub platform: Option<&'a str>,
    /// Build identifier of the running emulator core.
    pub core_build_id: Option<&'a str>,
    /// Engine version; callers normally use `ENGINE_VERSION`.
    pub engine_version: Option<&'a str>,
    /// Whether release signature policy must be enforced.
    pub release_build: bool,
}

/// Complete pack-validation report.
#[derive(Debug, Default, Clone)]
pub struct Report {
    /// Findings in validation order.
    pub findings: Vec<Finding>,
}

impl Report {
    /// Returns whether any finding prevents the pack from running.
    pub fn has_errors(&self) -> bool {
        self.findings.iter().any(|f| f.severity == Severity::Error)
    }
    /// Iterates over error findings.
    pub fn errors(&self) -> impl Iterator<Item = &Finding> {
        self.findings
            .iter()
            .filter(|f| f.severity == Severity::Error)
    }
    /// Iterates over advertencia findings.
    pub fn warnings(&self) -> impl Iterator<Item = &Finding> {
        self.findings
            .iter()
            .filter(|f| f.severity == Severity::Warning)
    }
    fn error(&mut self, code: &'static str, message: String) {
        self.findings.push(Finding {
            severity: Severity::Error,
            code,
            message,
        });
    }
    fn warn(&mut self, code: &'static str, message: String) {
        self.findings.push(Finding {
            severity: Severity::Warning,
            code,
            message,
        });
    }
    fn info(&mut self, code: &'static str, message: String) {
        self.findings.push(Finding {
            severity: Severity::Info,
            code,
            message,
        });
    }
    /// Iterates over informational recommendations.
    pub fn infos(&self) -> impl Iterator<Item = &Finding> {
        self.findings
            .iter()
            .filter(|f| f.severity == Severity::Info)
    }

    /// Serializes the report to stable JSON for tools and FFI consumers.
    pub fn to_json(&self) -> String {
        fn esc(s: &str) -> String {
            let mut o = String::with_capacity(s.len());
            for c in s.chars() {
                match c {
                    '"' => o.push_str("\\\""),
                    '\\' => o.push_str("\\\\"),
                    '\n' => o.push_str("\\n"),
                    c => o.push(c),
                }
            }
            o
        }
        let mut j = String::from("{\n  \"hallazgos\": [\n");
        for (i, f) in self.findings.iter().enumerate() {
            let sev = match f.severity {
                Severity::Error => "error",
                Severity::Warning => "advertencia",
                Severity::Info => "recomendacion",
            };
            j.push_str(&format!(
                "    {{\"severidad\": \"{}\", \"codigo\": \"{}\", \"mensaje\": \"{}\"}}{}\n",
                sev,
                f.code,
                esc(&f.message),
                if i + 1 < self.findings.len() { "," } else { "" }
            ));
        }
        j.push_str(&format!(
            "  ],\n  \"errores\": {},\n  \"advertencias\": {},\n  \"recomendaciones\": {},\n  \"valido\": {}\n}}\n",
            self.errors().count(), self.warnings().count(), self.infos().count(),
            if self.has_errors() { "false" } else { "true" }));
        j
    }
}

// -- Manifest tolerante ------------------------------------------------------
// TODO opcional, incluido lo que `AyArchive` exige. Un manifest al que le falta
// `nombre` es un hallazgo que hay que REPORTAR, no un parseo que falla y deja al
// usuario con «no se pudo abrir el pack».

#[derive(Deserialize, Default)]
struct LaxPack {
    name: Option<String>,
    ///  los tres que un catálogo necesita y que el validador de sesión no
    /// miraba porque no le hacían falta para CORRER el pack.
    author: Option<String>,
    description: Option<String>,
    license: Option<String>,
    version: Option<String>,
    game_id: Option<String>,
    ayther_min: Option<String>,
    schema: Option<u32>,
    format: Option<u32>,
}

#[derive(Deserialize, Default)]
struct LaxCompat {
    rom_crc32: Option<String>,
    platform: Option<String>,
    core_min: Option<String>,
}

#[derive(Deserialize, Default)]
struct LaxSystems {
    #[serde(default)]
    included: Vec<String>,
}

#[derive(Deserialize, Default)]
struct LaxManifest {
    #[serde(default)]
    pack: LaxPack,
    #[serde(default)]
    compat: Option<LaxCompat>,
    #[serde(default)]
    systems: Option<LaxSystems>,
}

/// "1.2.3" → (1,2,3). Lo que no parsea vale 0, así que una versión rara nunca
/// bloquea por accidente — el hallazgo lo genera la comparación, no el parseo.
fn semver(s: &str) -> (u32, u32, u32) {
    let mut it = s
        .trim()
        .split('.')
        .map(|p| p.trim().parse::<u32>().unwrap_or(0));
    (
        it.next().unwrap_or(0),
        it.next().unwrap_or(0),
        it.next().unwrap_or(0),
    )
}

// ---------------------------------------------------------------------------
// El GRADO de compatibilidad
// ---------------------------------------------------------------------------
//
// La misma pregunta se hace en tres lugares —el SDK antes de correr, Play antes
// de lanzar, el Hub antes de dejar descargar— y la peor forma de contestarla es
// tres veces. Esto es la ÚNICA implementación: deriva el grado del informe que
// ya produce `validate_path`, así que agregar una regla de validación mueve el
// grado en los tres lados a la vez, sin que nadie tenga que acordarse.
//
// LOS CUATRO GRADOS NO SON TRES NIVELES DE ERROR MÁS UNO BUENO. La distinción
// que sostiene la matriz es entre «se comprobó y está bien» y «no se pudo
// comprobar»: un pack que declara compatibilidad con una ROM cuyo CRC nadie
// aportó no es compatible ni incompatible — es **experimental**, y decirle
// «exacta» sería afirmar algo que no se midió. Es la misma doctrina que la
// ficha del pack en Play (declarado ≠ verificado).
//
// Por eso `Exact` exige contexto COMPLETO. Un consumidor que no sabe el CRC de
// la ROM del usuario no puede obtener «exacta» ni queriendo, y eso es una
// propiedad, no una limitación: la métrica del Hub («incompatibilidades
// bloqueadas antes de descargar = 100 %») se apoya en que nadie pueda decir
// «exacta» sin haber mirado.

/// Returns whether a finding code affects pack/session compatibility.
///
/// Keeping the classification in one place ensures all consumers assign the
/// same compatibility grado.
pub fn affects_compatibility(code: &str) -> bool {
    matches!(
        code.split('.').next().unwrap_or(""),
        // La sesión concreta: la ROM, la plataforma, el core, el motor.
        "rom" | "platform" | "core" | "engine" | "format" | "schema" | "systems"
        // El pack como ARCHIVO: si esto falla, no hay nada que correr.
        | "manifest" | "integrity" | "catalog" | "zip" | "io" | "signature"
    )
}

/// Compatibility grado between a pack and a concrete runtime session.
///
/// Declaration order is best to worst and forms part of the comparison contract.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum CompatGrade {
    /// Every declared requirement was verified and matched.
    Exact,
    /// Verified and usable, with warnings but no blocking errors.
    Warnings,
    /// Usability could not be fully verified because session context is missing.
    Experimental,
    /// At least one error prevents the pack from running in this session.
    Incompatible,
}

impl CompatGrade {
    /// Returns the stable identifier used by JSON and external APIs.
    pub fn id(self) -> &'static str {
        match self {
            CompatGrade::Exact => "exact",
            CompatGrade::Warnings => "warnings",
            CompatGrade::Experimental => "experimental",
            CompatGrade::Incompatible => "incompatible",
        }
    }
    /// Returns whether the pack may be attempted in the current session.
    ///
    /// Only [`CompatGrade::Incompatible`] blocks execution.
    pub fn runnable(self) -> bool {
        self != CompatGrade::Incompatible
    }
}

/// Compatibility grado together with its rationale and supporting report.
#[derive(Debug, Clone)]
pub struct CompatVerdict {
    /// Overall compatibility classification.
    pub grade: CompatGrade,
    /// Human-readable rationale; never empty.
    pub reason: String,
    /// Names of session facts that were unavailable for verification.
    pub unverified: Vec<&'static str>,
    /// Detailed findings that support the verdict.
    pub report: Report,
}

impl CompatVerdict {
    /// Serializes the verdict and its full report to stable JSON.
    pub fn to_json(&self) -> String {
        fn esc(s: &str) -> String {
            s.replace('\\', "\\\\").replace('\"', "\\\"")
        }
        let unv: Vec<String> = self
            .unverified
            .iter()
            .map(|u| format!("\"{}\"", u))
            .collect();
        format!(
            "{{\"grado\":\"{}\",\"ejecutable\":{},\"motivo\":\"{}\",\
             \"sin_verificar\":[{}],\"informe\":{}}}",
            self.grade.id(),
            self.grade.runnable(),
            esc(&self.reason),
            unv.join(","),
            self.report.to_json()
        )
    }
}

/// Computes the compatibility grado of a pack for one runtime session.
///
/// The function validates archive metadata without loading the pack for play.
pub fn compat_grade(path: &str, ctx: &SessionCtx) -> CompatVerdict {
    let report = validate_path(path, ctx);

    // Lo que el CALLER no aportó. Se mira el contexto y no el informe: un
    // hallazgo puede faltar porque el pack no exige ese requisito, y ahí no
    // haber aportado el dato no es una laguna. Lo que hace falta comprobar lo
    // decide el pack; lo que se pudo comprobar, el contexto.
    let mut unverified: Vec<&'static str> = Vec::new();
    if ctx.rom_crc32.is_none() {
        unverified.push("rom_crc32");
    }
    if ctx.platform.is_none() {
        unverified.push("platform");
    }
    if ctx.core_build_id.is_none() {
        unverified.push("core_build_id");
    }

    // Sólo los hallazgos de COMPATIBILIDAD: los de publicación (autor,
    // licencia, procedencia, `assets/` vacío) dicen «no está listo para
    // publicar», no «no corre con tu ROM». Mezclarlos degradaría el grado de un
    // pack que anda perfecto y entrenaría a ignorarlo.
    let n_err = report
        .errors()
        .filter(|f| affects_compatibility(f.code))
        .count();
    let n_warn = report
        .warnings()
        .filter(|f| affects_compatibility(f.code))
        .count();

    // El ORDEN de las ramas es el contrato. Un error gana sobre cualquier cosa
    // que no se haya podido comprobar: si ya sabemos que no corre, decir
    // «experimental» sería suavizar un no.
    let (grade, reason) = if n_err > 0 {
        let first_message = report
            .errors()
            .find(|f| affects_compatibility(f.code))
            .map(|f| f.message.clone())
            .unwrap_or_default();
        (
            CompatGrade::Incompatible,
            format!(
                "{} error(es) impiden usar el pack en esta sesión: {}",
                n_err, first_message
            ),
        )
    } else if !unverified.is_empty() {
        (
            CompatGrade::Experimental,
            format!(
                "no se pudo comprobar: {}. Se puede intentar; que ande no está \
                  verificado",
                unverified.join(", ")
            ),
        )
    } else if n_warn > 0 {
        (
            CompatGrade::Warnings,
            format!("compatible con {} advertencia(s): algo va a faltar", n_warn),
        )
    } else {
        (
            CompatGrade::Exact,
            "todo lo que el pack exige se comprobó y coincide".to_string(),
        )
    };

    CompatVerdict {
        grade,
        reason,
        unverified,
        report,
    }
}

/// Validates an `.ay` archivo without opening it for play.
///
/// Every failure becomes a structured finding in the returned report.
pub fn validate_path(path: &str, ctx: &SessionCtx) -> Report {
    let mut r = Report::default();

    let file = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(e) => {
            r.error("io", format!("no se pudo abrir '{}': {}", path, e));
            return r;
        }
    };
    let archive_size = match file.metadata() {
        Ok(metadata) => metadata.len(),
        Err(e) => {
            r.error("io", format!("no se pudo medir el pack '{}': {}", path, e));
            return r;
        }
    };
    if let Err(e) = validate_archive_size(archive_size) {
        r.error("security.policy", e.to_string());
        return r;
    }
    let mut zip = match zip::ZipArchive::new(file) {
        Ok(z) => z,
        Err(e) => {
            r.error("zip", format!("no es un .ay legible (ZIP inválido): {}", e));
            return r;
        }
    };

    let mut central_entries = Vec::with_capacity(zip.len());
    for i in 0..zip.len() {
        let entry = match zip.by_index_raw(i) {
            Ok(entry) => entry,
            Err(e) => {
                r.error(
                    "zip",
                    format!("el directorio central del pack no se puede leer: {e}"),
                );
                return r;
            }
        };
        if !entry.is_dir() {
            central_entries.push((
                entry.name().to_string(),
                entry.size(),
                entry.compressed_size(),
            ));
        }
    }
    if let Err(e) = validate_archive_metadata(
        archive_size,
        zip.len(),
        central_entries
            .iter()
            .map(|(path, size, compressed)| EntryMetadata {
                path,
                uncompressed_size: *size,
                compressed_size: *compressed,
            }),
    ) {
        r.error("security.policy", e.to_string());
        return r;
    }
    let names: Vec<String> = central_entries
        .into_iter()
        .map(|(path, _, _)| path)
        .collect();

    // -- manifest.toml -------------------------------------------------------
    let mut manifest_txt = String::new();
    match zip.by_name("manifest.toml") {
        Ok(mut e) => {
            let _ = e.read_to_string(&mut manifest_txt);
        }
        Err(_) => {
            r.error(
                "manifest.missing",
                "el pack no trae manifest.toml — no hay forma de saber \
                     de qué juego es"
                    .to_string(),
            );
            return r; // sin manifest no queda nada que comparar
        }
    }
    let m: LaxManifest = match toml::from_str(&manifest_txt) {
        Ok(m) => m,
        Err(e) => {
            r.error(
                "manifest.malformed",
                format!("manifest.toml no se puede leer: {}", e),
            );
            return r;
        }
    };

    // -- formato del contenedor ----------------------------------------------
    // A future container may change the meaning of the remaining manifest, so
    // do not derive secondary findings from fields this build cannot interpret.
    let format = m.pack.format.unwrap_or(1);
    if format > PACK_FORMAT {
        r.error(
            "format.newer",
            format!(
                "el pack declara formato de contenedor {} y este AYTHER entiende hasta {} \
                 — hay que actualizar",
                format, PACK_FORMAT
            ),
        );
        return r;
    }

    // -- esquema -------------------------------------------------------------
    let schema = m.pack.schema.unwrap_or(1);
    if schema > MANIFEST_SCHEMA {
        r.error(
            "schema.newer",
            format!(
                "el pack declara esquema {} y este AYTHER entiende hasta {} \
                     — hay que actualizar",
                schema, MANIFEST_SCHEMA
            ),
        );
    }

    // -- campos obligatorios -------------------------------------------------
    if m.pack.name.as_deref().unwrap_or("").is_empty() {
        r.error("pack.nombre", "el manifest no declara `nombre`".to_string());
    }
    if m.pack.version.as_deref().unwrap_or("").is_empty() {
        r.warn(
            "pack.version",
            "el manifest no declara `version`".to_string(),
        );
    }
    if m.pack.game_id.as_deref().unwrap_or("").is_empty() {
        r.warn(
            "pack.game_id",
            "el manifest no declara `game_id`: los mensajes de error no van a \
             poder nombrar el juego"
                .to_string(),
        );
    }

    // -- versión mínima de AYTHER --------------------------------------------
    let engine = ctx.engine_version.unwrap_or(ENGINE_VERSION);
    if let Some(min) = m.pack.ayther_min.as_deref()
        && semver(min) > semver(engine)
    {
        r.error(
            "engine.too_old",
            format!(
                "el pack pide AYTHER {} o superior y este es {}",
                min, engine
            ),
        );
    }

    // -- compatibilidad de ROM y plataforma ----------------------------------
    let compat = m.compat.unwrap_or_default();

    match (compat.rom_crc32.as_deref(), ctx.rom_crc32) {
        (Some(decl), Some(actual)) => {
            let d = u32::from_str_radix(decl.trim().trim_start_matches("0x"), 16);
            match d {
                Ok(d) if d == actual => {}
                Ok(d) => r.error(
                    "rom.mismatch",
                    format!(
                        "el pack se horneó para la ROM {:08x} y la sesión \
                             tiene {:08x} — es de otro juego (o de otra \
                             revisión)",
                        d, actual
                    ),
                ),
                Err(_) => r.warn(
                    "rom.unreadable",
                    format!("`compat.rom_crc32` no es hexadecimal: '{}'", decl),
                ),
            }
        }
        // Declarado y la sesión no lo sabe, o al revés: no se puede verificar.
        // Es una advertencia y no un error — decirlo es lo que evita que el
        // silencio se lea como «verificado».
        (Some(_), None) => r.warn(
            "rom.unverified",
            "el pack declara una ROM pero la sesión no dice cuál cargó: no se \
             pudo verificar que coincidan"
                .to_string(),
        ),
        (None, _) => r.warn(
            "rom.undeclared",
            "el pack no declara contra qué ROM se horneó: si es de otro juego, \
             se va a notar recién al jugarlo"
                .to_string(),
        ),
    }

    if let (Some(decl), Some(actual)) = (compat.platform.as_deref(), ctx.platform)
        && !decl.eq_ignore_ascii_case(actual)
    {
        r.error(
            "platform.mismatch",
            format!("el pack es de {} y la sesión es {}", decl, actual),
        );
    }

    // El core se compara por IGUALDAD y no por orden: un `build_id` no es una
    // versión ordenable. Por eso es advertencia — que sea otro no prueba que no
    // funcione, sólo que no es contra el que se horneó.
    if let (Some(decl), Some(actual)) = (compat.core_min.as_deref(), ctx.core_build_id)
        && decl != actual
    {
        r.warn(
            "core.different",
            format!(
                "el pack se horneó con el core '{}' y este es '{}'",
                decl, actual
            ),
        );
    }

    // -- subsistemas que este build no conoce --------------------------------
    // Degradación opcional, no error: el pack trae algo que este AYTHER no sabe
    // aplicar, y todo lo demás sigue sirviendo.
    if let Some(sys) = m.systems {
        for s in &sys.included {
            if !crate::archive_vfs::SUBSYSTEMS.iter().any(|k| k == s) {
                r.warn(
                    "systems.unknown",
                    format!(
                        "el pack trae el subsistema '{}', que este AYTHER \
                             no sabe aplicar",
                        s
                    ),
                );
            }
        }
    }

    // -- firma e integridad --------------------------------------------------
    let signed = names.iter().any(|n| n == "signature.bin");
    if !signed {
        if ctx.release_build {
            r.error(
                "signature.missing",
                "el pack no está firmado y este es un build de release".to_string(),
            );
        } else {
            r.warn(
                "signature.missing",
                "el pack no está firmado (se acepta sólo en builds de \
                 desarrollo)"
                    .to_string(),
            );
        }
    }

    // integrity.toml: que cada entrada declarada EXISTA. No se verifican los
    // hashes acá —eso es leer el pack entero, y esta función tiene que ser
    // barata para poder correr antes de cada sesión—; el hash por entrada lo
    // verifica la lectura, que es donde importa.
    if names.iter().any(|n| n == "integrity.toml") {
        let mut txt = String::new();
        if let Ok(mut e) = zip.by_name("integrity.toml") {
            let _ = e.read_to_string(&mut txt);
        }
        #[derive(Deserialize, Default)]
        struct LaxEntry {
            path: String,
        }
        #[derive(Deserialize, Default)]
        struct LaxIntegrity {
            #[serde(default)]
            entry: Vec<LaxEntry>,
        }
        match toml::from_str::<LaxIntegrity>(&txt) {
            Ok(idx) => {
                if idx.entry.len() > MAX_ENTRY_COUNT {
                    r.error(
                        "security.policy",
                        format!(
                            "integrity.toml declara {} entradas; el máximo es {MAX_ENTRY_COUNT}",
                            idx.entry.len()
                        ),
                    );
                    return r;
                }
                let mut indexed_paths = HashSet::with_capacity(idx.entry.len());
                for entry in &idx.entry {
                    if let Err(e) = validate_canonical_logical_path(&entry.path) {
                        r.error("security.policy", e.to_string());
                        return r;
                    }
                    if matches!(entry.path.as_str(), "integrity.toml" | "signature.bin")
                        || !indexed_paths.insert(entry.path.to_ascii_lowercase())
                    {
                        r.error(
                            "security.policy",
                            format!(
                                "integrity.toml repite o intenta indexar una ruta reservada: '{}'",
                                entry.path
                            ),
                        );
                        return r;
                    }
                }
                let missing: Vec<&str> = idx
                    .entry
                    .iter()
                    .map(|e| e.path.as_str())
                    .filter(|p| !names.iter().any(|n| n == p))
                    .collect();
                if !missing.is_empty() {
                    r.error(
                        "integrity.missing_entry",
                        format!(
                            "el índice firmado declara {} archivo(s) que no \
                                 están en el pack (el primero: '{}')",
                            missing.len(),
                            missing[0]
                        ),
                    );
                }
            }
            Err(e) => r.error(
                "integrity.malformed",
                format!("integrity.toml no se puede leer: {}", e),
            ),
        }
    } else {
        r.warn(
            "integrity.legacy",
            "pack legacy sin integrity.toml: se verifica entero al abrir, no \
             por asset"
                .to_string(),
        );
    }

    // --  los chequeos de AUTORÍA -------------------------------------
    //
    // Los de arriba contestan «¿este pack corre en ESTA sesión?». Estos otros
    // contestan «¿este pack está listo para publicarse?», que es la pregunta
    // que un autor tiene antes de subirlo y la que el Hub le va a contestar
    // después (HUB-EP03.2). Están acá y no en el Lab para que la respuesta sea
    // LA MISMA — un autor que pasa el validador del SDK no puede llevarse una
    // sorpresa estructural en la ingesta.
    //
    // Ninguno es error: un proyecto a medio hacer se tiene que poder empaquetar
    // y probar. Lo que no puede es publicarse creyendo que está completo.
    {
        // Metadatos que un catálogo necesita para mostrar el pack. Sin autor no
        // hay a quién atribuirle nada, y sin descripción la ficha queda vacía.
        if m.pack.author.as_deref().unwrap_or("").trim().is_empty() {
            r.warn(
                "meta.author",
                "el manifest no declara `author` — el pack no puede atribuirse \
                 a nadie"
                    .to_string(),
            );
        }
        if m.pack
            .description
            .as_deref()
            .unwrap_or("")
            .trim()
            .is_empty()
        {
            r.info(
                "meta.description",
                "sin `description`, la ficha del pack queda sin texto".to_string(),
            );
        }

        // LICENCIA. Es advertencia y no recomendación: sin licencia declarada,
        // quien lo descargue no sabe qué puede hacer con él — y eso no es un
        // detalle de presentación.
        let license = m.pack.license.as_deref().unwrap_or("").trim().to_string();
        if license.is_empty() {
            r.warn(
                "meta.license",
                "el manifest no declara `license`: nadie que lo descargue sabe \
                 qué puede hacer con el contenido"
                    .to_string(),
            );
        }

        // Require provenance when the license requires attribution. Declaring
        // CC BY without naming a recipient describes an unusable permission.
        let requires_attribution = license.to_uppercase().contains("BY");
        let has_credits = names.iter().any(|n| n == "credits.toml");
        if requires_attribution && !has_credits {
            r.error(
                "provenance.missing",
                format!(
                    "la licencia declarada ({}) exige atribución y el pack \
                         no trae credits.toml: es un permiso que nadie puede \
                         cumplir",
                    license
                ),
            );
        } else if !has_credits {
            r.info(
                "provenance.absent",
                "el pack no trae credits.toml: no se puede mostrar quién hizo \
                 cada cosa"
                    .to_string(),
            );
        }

        // ASSETS. Un pack cuyo único contenido es el manifest no sustituye
        // nada — se puede publicar, pero casi seguro no es lo que el autor
        // quiso.
        let assets = names.iter().filter(|n| n.starts_with("assets/")).count();
        if assets == 0 {
            r.warn(
                "assets.empty",
                "el pack no trae ninguna entrada bajo assets/: no sustituye \
                 nada"
                    .to_string(),
            );
        }

        // Los TOML del catálogo tienen que ser LEGIBLES. Uno roto no rompe el
        // pack —el motor lo saltea— pero se lleva en silencio todo lo que
        // declaraba, que es la peor forma de perder trabajo autorado.
        for cat in [
            "elements.toml",
            "acetatos.toml",
            "audio_events.toml",
            "panoramas.toml",
            "animations.toml",
            "credits.toml",
            "instruments.toml",
            "game_profile.toml",
            "entity_substitutions.toml",
        ] {
            if !names.iter().any(|n| n == cat) {
                continue;
            }
            let mut txt = String::new();
            if zip
                .by_name(cat)
                .map(|mut e| e.read_to_string(&mut txt))
                .is_err()
            {
                continue;
            }
            if toml::from_str::<toml::Value>(&txt).is_err() {
                r.error(
                    "catalog.malformed",
                    format!(
                        "{} no se puede leer como TOML: todo lo que declara \
                             se pierde en silencio",
                        cat
                    ),
                );
            }
        }
    }

    r
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;
    use crate::pack_builder::PackBuilder;

    fn bake(name: &str, manifest: &str) -> String {
        let dir = std::env::temp_dir().join("ay_validate_tests");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join(format!("{}.ay", name));
        let mut b = PackBuilder::new();
        b.add_bytes("manifest.toml", manifest.as_bytes().to_vec());
        b.add_bytes("graphics/a.png", vec![0x89, 0x50, 0x4E, 0x47]);
        b.finish(true, &path).unwrap();
        path.to_str().unwrap().to_string()
    }

    const OK_MANIFEST: &str = r#"
[pack]
name         = "Test"
version    = "1.0.0"
game_id    = "crc32:7b905383"
ayther_min = "0.1.0"
schema     = 2

[compat]
rom_crc32 = "7b905383"
platform  = "megadrive"
"#;

    /// El caso bueno no genera NINGÚN error. Va primero porque un validador que
    /// se queja de todo es tan inútil como uno que no se queja de nada.
    #[test]
    fn valid_pack_has_no_errors() {
        let p = bake("ok", OK_MANIFEST);
        let ctx = SessionCtx {
            rom_crc32: Some(0x7b905383),
            platform: Some("megadrive"),
            ..Default::default()
        };
        let r = validate_path(&p, &ctx);
        assert!(!r.has_errors(), "hallazgos: {:?}", r.findings);
    }

    /// Rejects a pack authored for a different game.
    #[test]
    fn wrong_rom_is_reported_as_error() {
        let p = bake("rom", OK_MANIFEST);
        let ctx = SessionCtx {
            rom_crc32: Some(0xDEADBEEF),
            ..Default::default()
        };
        let r = validate_path(&p, &ctx);
        assert!(r.has_errors());
        assert!(
            r.errors().any(|f| f.code == "rom.mismatch"),
            "hallazgos: {:?}",
            r.findings
        );
    }

    /// …y NO poder verificarlo es una advertencia, no un error. Si fuera error,
    /// ningún pack sin `rom_crc32` (todos los de antes de ) abriría.
    #[test]
    fn undeclared_rom_is_warning() {
        let p = bake(
            "without_rom",
            r#"
[pack]
name         = "Test"
version    = "1.0.0"
game_id    = "sonic2"
ayther_min = "0.1.0"
"#,
        );
        let ctx = SessionCtx {
            rom_crc32: Some(0x7b905383),
            ..Default::default()
        };
        let r = validate_path(&p, &ctx);
        assert!(!r.has_errors(), "hallazgos: {:?}", r.findings);
        assert!(r.warnings().any(|f| f.code == "rom.undeclared"));
    }

    #[test]
    fn old_engine_is_error() {
        let p = bake(
            "engine",
            r#"
[pack]
name         = "Del futuro"
version    = "1.0.0"
game_id    = "sonic2"
ayther_min = "99.0.0"
"#,
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(
            r.errors().any(|f| f.code == "engine.too_old"),
            "hallazgos: {:?}",
            r.findings
        );
    }

    /// Un esquema más nuevo se REPORTA en vez de reventar. Es la diferencia
    /// entre esta función y `AyArchive::open`, que devuelve None sin explicar.
    #[test]
    fn newer_schema_is_reported_without_crash() {
        let p = bake(
            "schema",
            &format!(
                r#"
[pack]
name         = "Del futuro"
version    = "1.0.0"
game_id    = "sonic2"
schema     = {}
"#,
                MANIFEST_SCHEMA + 1
            ),
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(
            r.errors().any(|f| f.code == "schema.newer"),
            "hallazgos: {:?}",
            r.findings
        );
    }

    #[test]
    fn newer_format_is_reported_as_incompatible() {
        let p = bake(
            "format",
            &format!(
                r#"
[pack]
name       = "Future container"
version    = "9.0.0"
game_id    = "sonic2"
format     = {}
"#,
                PACK_FORMAT + 1
            ),
        );
        let verdict = compat_grade(&p, &SessionCtx::default());
        assert_eq!(verdict.grade, CompatGrade::Incompatible);
    }

    #[test]
    fn format_diagnostic_takes_priority_over_schema() {
        let p = bake(
            "format_before_schema",
            &format!(
                r#"
[pack]
name       = "Future container and schema"
version    = "9.0.0"
game_id    = "sonic2"
format     = {}
schema     = {}
"#,
                PACK_FORMAT + 1,
                MANIFEST_SCHEMA + 1
            ),
        );
        let r = validate_path(&p, &SessionCtx::default());
        let error_codes: Vec<_> = r.errors().map(|f| f.code).collect();
        assert_eq!(error_codes, vec!["format.newer"]);
    }

    /// Un subsistema desconocido NO impide correr: es degradación opcional, y
    /// tratarla como error dejaría fuera packs perfectamente usables.
    #[test]
    fn unknown_subsystem_is_warning() {
        let p = bake(
            "sys",
            r#"
[pack]
name         = "Test"
version    = "1.0.0"
game_id    = "sonic2"
schema     = 2

[systems]
included = ["sprites", "holograma"]
"#,
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(!r.has_errors(), "hallazgos: {:?}", r.findings);
        assert!(r.warnings().any(|f| f.code == "systems.unknown"));
    }

    /// Un archivo que no es un pack no puede tirar la sesión: sale como
    /// hallazgo, igual que todo lo demás.
    #[test]
    fn arbitrary_file_does_not_crash() {
        let dir = std::env::temp_dir().join("ay_validate_tests");
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("basura.ay");
        std::fs::write(&p, b"esto no es un zip").unwrap();
        let r = validate_path(p.to_str().unwrap(), &SessionCtx::default());
        assert!(r.has_errors());
        assert!(
            r.errors().any(|f| f.code == "zip"),
            "hallazgos: {:?}",
            r.findings
        );
    }

    #[test]
    fn missing_pack_does_not_crash() {
        let r = validate_path("no/existe/nada.ay", &SessionCtx::default());
        assert!(r.errors().any(|f| f.code == "io"));
    }
    // -- El grado de compatibilidad ----------------------------------
    //
    // Lo que estos casos fijan no es «hay cuatro valores» sino la distinción
    // que sostiene la matriz: comprobado-y-bien contra no-se-pudo-comprobar.
    // Un enum de cuatro variantes donde `Experimental` nunca sale es un enum
    // de tres.

    fn complete_context<'a>() -> SessionCtx<'a> {
        SessionCtx {
            rom_crc32: Some(0x7b90_5383),
            platform: Some("megadrive"),
            core_build_id: Some("test-build"),
            engine_version: None,
            release_build: false,
        }
    }

    /// Contexto completo y sin hallazgos: exacta. Es el único camino a `Exact`,
    /// y por eso va primero.
    #[test]
    fn complete_clean_context_is_exact() {
        let p = bake("grade_exact", OK_MANIFEST);
        let v = compat_grade(&p, &complete_context());
        assert_eq!(
            v.grade,
            CompatGrade::Exact,
            "motivo: {} · {:?}",
            v.reason,
            v.report.findings
        );
        assert!(v.unverified.is_empty());
        assert!(v.grade.runnable());
        assert!(
            !v.reason.is_empty(),
            "un grado sin motivo obliga a inventar el texto"
        );
    }

    /// EL CASO QUE DA SENTIDO A LA MATRIZ: el mismo pack, el mismo veredicto de
    /// validación, y sin el CRC de la ROM baja a experimental. Nadie puede
    /// decir «exacta» sin haber mirado — de eso depende la métrica del Hub
    /// «incompatibilidades bloqueadas antes de descargar = 100 %».
    #[test]
    fn missing_context_is_experimental() {
        let p = bake("grade_exp", OK_MANIFEST);
        let v = compat_grade(
            &p,
            &SessionCtx {
                rom_crc32: None,
                ..complete_context()
            },
        );
        assert_eq!(v.grade, CompatGrade::Experimental);
        assert!(
            v.unverified.contains(&"rom_crc32"),
            "y DICE qué falta, para que el consumidor pueda pedirlo"
        );
        assert!(v.grade.runnable(), "experimental se puede intentar");

        // Sin nada de contexto, las tres cosas quedan sin verificar.
        let v2 = compat_grade(&p, &SessionCtx::default());
        assert_eq!(v2.grade, CompatGrade::Experimental);
        assert_eq!(v2.unverified.len(), 3);
    }

    /// Un error gana sobre cualquier cosa que no se haya podido comprobar: si
    /// ya sabemos que no corre, decir «experimental» sería suavizar un no.
    #[test]
    fn error_overrides_unverified_state() {
        let p = bake("grade_bad", OK_MANIFEST);
        // ROM que no es la del pack Y sin plataforma ni core: hay error y hay
        // cosas sin verificar a la vez.
        let v = compat_grade(
            &p,
            &SessionCtx {
                rom_crc32: Some(0xDEAD_BEEF),
                ..SessionCtx::default()
            },
        );
        assert_eq!(v.grade, CompatGrade::Incompatible);
        assert!(!v.grade.runnable(), "incompatible es el único que bloquea");
        assert!(v.report.errors().any(|f| f.code == "rom.mismatch"));
        // El motivo nombra el primer error, no un texto genérico.
        assert!(v.reason.len() > 20);
    }

    /// El orden del enum es contrato: un consumidor compara grados para decidir
    /// («<= Warnings» = se puede jugar sin sorpresas grandes).
    #[test]
    fn grades_are_ordered_best_to_worst() {
        assert!(CompatGrade::Exact < CompatGrade::Warnings);
        assert!(CompatGrade::Warnings < CompatGrade::Experimental);
        assert!(CompatGrade::Experimental < CompatGrade::Incompatible);
    }

    /// El JSON lleva el grado, si se puede correr, el motivo, lo que faltó y el
    /// informe entero — para que una herramienta no tenga que validar dos veces.
    #[test]
    fn json_is_stable_and_contains_report() {
        let p = bake("grade_json", OK_MANIFEST);
        let j = compat_grade(&p, &SessionCtx::default()).to_json();
        assert!(j.contains("\"grado\":\"experimental\""));
        assert!(j.contains("\"ejecutable\":true"));
        assert!(j.contains("\"rom_crc32\""));
        assert!(j.contains("\"informe\":"));
        // Y es JSON de verdad: las comillas del motivo van escapadas.
        assert_eq!(j.matches("{").count(), j.matches("}").count());
    }

    /// Los cuatro ids son estables y distintos: son lo que las herramientas
    /// comparan, y dos iguales harían indistinguibles dos grados.
    #[test]
    fn four_grade_ids_are_distinct() {
        let ids = [
            CompatGrade::Exact.id(),
            CompatGrade::Warnings.id(),
            CompatGrade::Experimental.id(),
            CompatGrade::Incompatible.id(),
        ];
        let mut u = ids.to_vec();
        u.sort();
        u.dedup();
        assert_eq!(u.len(), 4);
    }
}

// ---------------------------------------------------------------------------
// — las fixtures ROTAS A PROPÓSITO
//
// Each failure uses its own stable code instead of a generic invalid-pack result,
// so authors can repair the specific defect without rebuilding blindly.
//
// Cada caso rompe UNA cosa sobre un pack que por lo demás está sano — con dos
// roturas a la vez no se sabe cuál produjo el hallazgo. Y el primer test fija
// el CONTROL: el pack sano no produce ningún error, así que un hallazgo en los
// otros es de la rotura y no del molde.
// ---------------------------------------------------------------------------
#[cfg(test)]
mod broken_fixtures {
    use super::*;
    use std::io::Write as _;

    const VALID_MANIFEST: &str = r#"
[pack]
name = "Pack Sano"
version = "1.0.0"
game_id = "juego"
ayther_min = "0.1.0"
schema = 2
author = "Alguien"
description = "Un pack de prueba"
license = "CC0-1.0"

[regions]
default = "NTSC"
supported = ["NTSC"]
"#;

    /// Escribe un `.ay` con las entradas dadas. Sin firmar y sin
    /// `integrity.toml` salvo que se pidan: lo que se prueba acá es el
    /// VALIDADOR, no el sellado.
    fn pack(dir: &std::path::Path, name: &str, entries: &[(&str, &[u8])]) -> String {
        let path = dir.join(name);
        let f = std::fs::File::create(&path).unwrap();
        let mut z = zip::ZipWriter::new(f);
        let opts: zip::write::FileOptions<()> = zip::write::FileOptions::default();
        for (n, d) in entries {
            z.start_file(*n, opts).unwrap();
            z.write_all(d).unwrap();
        }
        z.finish().unwrap();
        path.to_string_lossy().into_owned()
    }

    fn valid_pack(dir: &std::path::Path, name: &str) -> String {
        pack(
            dir,
            name,
            &[
                ("manifest.toml", VALID_MANIFEST.as_bytes()),
                ("assets/a.png", &[1, 2, 3]),
            ],
        )
    }

    fn codes(r: &Report) -> Vec<&'static str> {
        r.findings.iter().map(|f| f.code).collect()
    }

    /// EL CONTROL. Sin esto, los tests de abajo pasarían aunque el validador
    /// marcara todo mal siempre.
    #[test]
    fn valid_pack_fixture_has_no_errors() {
        let d = tempfile::tempdir().unwrap();
        let r = validate_path(&valid_pack(d.path(), "sano.ay"), &SessionCtx::default());
        assert!(
            !r.has_errors(),
            "el pack sano no puede dar errores: {:?}",
            codes(&r)
        );
    }

    #[test]
    fn missing_manifest() {
        let d = tempfile::tempdir().unwrap();
        let p = pack(d.path(), "sin_manifest.ay", &[("assets/a.png", &[1])]);
        let r = validate_path(&p, &SessionCtx::default());
        assert!(codes(&r).contains(&"manifest.missing"), "{:?}", codes(&r));
    }

    #[test]
    fn invalid_toml() {
        let d = tempfile::tempdir().unwrap();
        let p = pack(
            d.path(),
            "roto.ay",
            &[("manifest.toml", b"esto no [[[ es toml")],
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(codes(&r).contains(&"manifest.malformed"), "{:?}", codes(&r));
    }

    /// Rejects a license that requires attribution when no contributor exists.
    #[test]
    fn missing_required_provenance() {
        let d = tempfile::tempdir().unwrap();
        let man = VALID_MANIFEST.replace("CC0-1.0", "CC-BY-4.0");
        let p = pack(
            d.path(),
            "sin_creditos.ay",
            &[("manifest.toml", man.as_bytes()), ("assets/a.png", &[1])],
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(codes(&r).contains(&"provenance.missing"), "{:?}", codes(&r));
        assert!(r.has_errors());

        // Y con los créditos puestos, el mismo pack pasa: el hallazgo es por la
        // FALTA, no por la licencia.
        let p2 = pack(
            d.path(),
            "con_creditos.ay",
            &[
                ("manifest.toml", man.as_bytes()),
                ("assets/a.png", &[1]),
                ("credits.toml", b"[[credit]]\nauthor = \"Ana\"\n"),
            ],
        );
        let r2 = validate_path(&p2, &SessionCtx::default());
        assert!(
            !codes(&r2).contains(&"provenance.missing"),
            "{:?}",
            codes(&r2)
        );
    }

    #[test]
    fn unreadable_catalog() {
        let d = tempfile::tempdir().unwrap();
        let p = pack(
            d.path(),
            "catalogo.ay",
            &[
                ("manifest.toml", VALID_MANIFEST.as_bytes()),
                ("assets/a.png", &[1]),
                ("acetatos.toml", b"[[acetato]] nombre = ROTO sin comillas ["),
            ],
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(codes(&r).contains(&"catalog.malformed"), "{:?}", codes(&r));
    }

    #[test]
    fn index_declares_missing_file() {
        let d = tempfile::tempdir().unwrap();
        let p = pack(
            d.path(),
            "indice.ay",
            &[
                ("manifest.toml", VALID_MANIFEST.as_bytes()),
                (
                    "integrity.toml",
                    b"[[entry]]\npath = \"assets/fantasma.png\"\nsha256 = \"00\"\nsize = 1\n",
                ),
            ],
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(
            codes(&r).contains(&"integrity.missing_entry"),
            "{:?}",
            codes(&r)
        );
    }

    #[test]
    fn missing_author_and_assets_warns() {
        let d = tempfile::tempdir().unwrap();
        let man = VALID_MANIFEST.replace("author = \"Alguien\"", "author = \"\"");
        let p = pack(d.path(), "flaco.ay", &[("manifest.toml", man.as_bytes())]);
        let r = validate_path(&p, &SessionCtx::default());
        let c = codes(&r);
        assert!(c.contains(&"meta.author"), "{c:?}");
        assert!(c.contains(&"assets.empty"), "{c:?}");
        // Un proyecto a medio hacer se tiene que poder empaquetar y probar.
        assert!(!r.has_errors(), "no puede ser error: {c:?}");
    }

    /// Los TRES niveles existen y se distinguen. Sin esto, «tres niveles» sería
    /// una promesa del documento: bastaría con que todo saliera como Warning
    /// para que los demás tests siguieran pasando.
    #[test]
    fn three_severity_levels_are_distinct() {
        let d = tempfile::tempdir().unwrap();
        let man = VALID_MANIFEST
            .replace("description = \"Un pack de prueba\"", "description = \"\"")
            .replace("license = \"CC0-1.0\"", "license = \"\"");
        let p = pack(
            d.path(),
            "niveles.ay",
            &[
                ("manifest.toml", man.as_bytes()),
                ("assets/a.png", &[1]),
                ("acetatos.toml", b"roto ["),
            ],
        );
        let r = validate_path(&p, &SessionCtx::default());
        assert!(r.errors().count() > 0, "falta un error: {:?}", codes(&r));
        assert!(
            r.warnings().count() > 0,
            "falta una advertencia: {:?}",
            codes(&r)
        );
        assert!(
            r.infos().count() > 0,
            "falta una recomendación: {:?}",
            codes(&r)
        );
    }

    /// El JSON lleva el código, la severidad y el veredicto — que es lo que
    /// deja a una herramienta reaccionar por código y no por texto.
    #[test]
    fn json_is_machine_consumable() {
        let d = tempfile::tempdir().unwrap();
        let p = pack(d.path(), "json.ay", &[("assets/a.png", &[1])]);
        let j = validate_path(&p, &SessionCtx::default()).to_json();
        assert!(j.contains("\"codigo\": \"manifest.missing\""), "{j}");
        assert!(j.contains("\"severidad\": \"error\""), "{j}");
        assert!(j.contains("\"valido\": false"), "{j}");

        let js = validate_path(&valid_pack(d.path(), "ok.ay"), &SessionCtx::default()).to_json();
        assert!(js.contains("\"valido\": true"), "{js}");
    }

    #[test]
    fn unsafe_archive_path_is_a_security_error() {
        let d = tempfile::tempdir().unwrap();
        let p = pack(
            d.path(),
            "traversal.ay",
            &[
                ("manifest.toml", VALID_MANIFEST.as_bytes()),
                ("../outside.bin", b"hostile"),
            ],
        );
        let report = validate_path(&p, &SessionCtx::default());
        assert!(
            codes(&report).contains(&"security.policy"),
            "{:?}",
            codes(&report)
        );
    }

    #[test]
    fn duplicate_integrity_path_is_a_security_error() {
        let d = tempfile::tempdir().unwrap();
        let index = b"[[entry]]\npath='manifest.toml'\n[[entry]]\npath='MANIFEST.toml'\n";
        let p = pack(
            d.path(),
            "duplicate_index.ay",
            &[
                ("manifest.toml", VALID_MANIFEST.as_bytes()),
                ("integrity.toml", index),
            ],
        );
        let report = validate_path(&p, &SessionCtx::default());
        assert!(
            codes(&report).contains(&"security.policy"),
            "{:?}",
            codes(&report)
        );
    }
}
