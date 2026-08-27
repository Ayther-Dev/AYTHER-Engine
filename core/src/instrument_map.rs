//! Mapping between detected game instruments and SoundFont presets.
//!
//! The mapping preserves game-controlled timing while replacing the timbre of
//! individual voices during synthesis.

// `instruments.toml` — timbre del juego → preset de SoundFont.
//
// QUÉ MAPEA, y por qué a esa escala. La clave es el `instrument` que emite el
// detector de audio: el hash del patch FM (operadores + algoritmo), SIN la
// frecuencia, SIN el canal y SIN el volumen. Es identidad de TIMBRE, no de
// nota ni de evento — misma filosofía que el hash de sprite.
//
// Eso es lo que hace la autoría viable: medido sobre 60 s de Sonic
// (`tools/sf2_spike`), **30 timbres distintos cubren toda la música**. Una
// banda sonora entera son del orden de 10-30 asignaciones, no miles de eventos.
// La misma escala que las Poses: se asigna la pose, no el frame.
//
// DÓNDE ENCAJA. Es un eje COMPLEMENTARIO a la Secuencia, no un reemplazo. Una
// Secuencia cambia un pasaje entero por una grabación —hay que rearreglar y
// regrabar afuera, y sólo calza si el timing coincide. Con un preset el juego
// SIGUE tocando (su tempo, sus jingles, sus cortes cuando te matan) y se cambia
// sólo el timbre de una voz; no puede desincronizar, porque la fuente de tiempo
// es el juego. En la escalera de resolución, Secuencia > Instrumento.
//
// LO QUE NO CUBRE: el 49,5% del bus son escrituras al DAC (batería, voces
// digitalizadas). Eso es PCM y queda fuera del alcance de un SoundFont; sigue
// por la sustitución de samples que ya existe.

/// Assignment from one detected game timbre to a SoundFont preset.
#[derive(Clone, Debug, PartialEq)]
pub struct InstrumentSub {
    /// Hash of the detected FM patch.
    pub patch: u64,
    /// Artist-facing label; it does not participate in identity matching.
    pub name: String,
    /// Basename of the source SoundFont stored in the pack.
    pub soundfont: String,
    /// MIDI bank number.
    pub bank: u16,
    /// MIDI preset number.
    pub preset: u16,
    /// Additional transposition in semitones.
    pub transpose: i8,
    /// Linear gain applied to the synthesized voice.
    pub gain: f32,
}

impl InstrumentSub {
    /// Creates an assignment with neutral transposition and gain.
    pub fn new(patch: u64, soundfont: &str, bank: u16, preset: u16) -> Self {
        Self {
            patch,
            name: String::new(),
            soundfont: soundfont.to_string(),
            bank,
            preset,
            transpose: 0,
            gain: 1.0,
        }
    }
}

fn esc(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

/// Serializes instrument assignments to `instruments.toml` syntax.
pub fn instruments_to_toml(subs: &[InstrumentSub]) -> String {
    let mut s = String::new();
    s.push_str("# instruments.toml — game instrument to SoundFont preset\n");
    s.push_str("# patch: timbre hash (FM patch without frequency, channel, or volume).\n");
    s.push_str("# soundfont: source SF2 basename; only assigned presets are packed.\n\n");
    for e in subs {
        s.push_str("[[instrument]]\n");
        s.push_str(&format!("patch = \"0x{:016x}\"\n", e.patch));
        if !e.name.is_empty() {
            s.push_str(&format!("name = \"{}\"\n", esc(&e.name)));
        }
        s.push_str(&format!("soundfont = \"{}\"\n", esc(&e.soundfont)));
        s.push_str(&format!("bank = {}\n", e.bank));
        s.push_str(&format!("preset = {}\n", e.preset));
        // Sólo si difieren del default: así agregar estos campos no cambia un
        // byte de los proyectos que no los usan. Mismo criterio que `gap` de la
        // Cinemática y el `@offset` del video.
        if e.transpose != 0 {
            s.push_str(&format!("transpose = {}\n", e.transpose));
        }
        if (e.gain - 1.0).abs() > f32::EPSILON {
            s.push_str(&format!("gain = {}\n", e.gain));
        }
        s.push('\n');
    }
    s
}

/// Parses `instruments.toml`, skipping malformed entries while preserving valid
/// assignments.
pub fn instruments_from_toml(text: &str) -> Vec<InstrumentSub> {
    let mut out = Vec::new();
    let tbl: toml::Value = match toml::from_str(text) {
        Ok(t) => t,
        Err(_) => return out,
    };
    let arr = match tbl.get("instrument").and_then(|v| v.as_array()) {
        Some(a) => a,
        None => return out,
    };
    for e in arr {
        let patch = match e
            .get("patch")
            .and_then(|v| v.as_str())
            .and_then(parse_hex_u64)
        {
            Some(p) if p != 0 => p,
            _ => continue, // sin clave no hay asignación
        };
        let soundfont = match e.get("soundfont").and_then(|v| v.as_str()) {
            Some(s) if !s.is_empty() => s.to_string(),
            _ => continue, // sin origen tampoco
        };
        // `preset` es obligatorio; `bank` casi siempre es 0 y se tolera ausente.
        let preset = match e.get("preset").and_then(|v| v.as_integer()) {
            Some(p) if (0..=127).contains(&p) => p as u16,
            _ => continue,
        };
        let bank = e
            .get("bank")
            .and_then(|v| v.as_integer())
            .filter(|b| (0..=128).contains(b))
            .unwrap_or(0) as u16;
        let name = e
            .get("name")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let transpose = e
            .get("transpose")
            .and_then(|v| v.as_integer())
            .unwrap_or(0)
            .clamp(-48, 48) as i8;
        let gain = e
            .get("gain")
            .and_then(|v| v.as_float())
            .or_else(|| e.get("gain").and_then(|v| v.as_integer()).map(|i| i as f64))
            .unwrap_or(1.0)
            .clamp(0.0, 8.0) as f32;
        out.push(InstrumentSub {
            patch,
            name,
            soundfont,
            bank,
            preset,
            transpose,
            gain,
        });
    }
    out
}

fn parse_hex_u64(s: &str) -> Option<u64> {
    let t = s.trim();
    let t = t
        .strip_prefix("0x")
        .or_else(|| t.strip_prefix("0X"))
        .unwrap_or(t);
    u64::from_str_radix(t, 16).ok()
}

/// Groups unique `(bank, preset)` pairs by SoundFont in deterministic order.
pub fn soundfonts_used(subs: &[InstrumentSub]) -> Vec<(String, Vec<(u16, u16)>)> {
    let mut out: Vec<(String, Vec<(u16, u16)>)> = Vec::new();
    for s in subs {
        let key = (s.bank, s.preset);
        match out.iter_mut().find(|(f, _)| *f == s.soundfont) {
            Some((_, v)) => {
                if !v.contains(&key) {
                    v.push(key)
                }
            }
            None => out.push((s.soundfont.clone(), vec![key])),
        }
    }
    // Orden determinista: el mapeo se hornea a un archivo y dos horneados
    // iguales tienen que dar bytes iguales.
    out.sort_by(|a, b| a.0.cmp(&b.0));
    for (_, v) in out.iter_mut() {
        v.sort_unstable();
    }
    out
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let mut a = InstrumentSub::new(0x76f836368cd4fcb9, "Cave Story.sf2", 0, 38);
        a.name = "Bajo".into();
        let mut b = InstrumentSub::new(0xe35bbd4294db943a, "gm.sf2", 0, 80);
        b.transpose = -12;
        b.gain = 0.75;
        let c = InstrumentSub::new(0x00000000000000ff, "gm.sf2", 8, 1);

        let subs = vec![a, b, c];
        let back = instruments_from_toml(&instruments_to_toml(&subs));
        assert_eq!(back, subs, "el mapeo tiene que volver idéntico");
    }

    /// Los defaults NO se escriben: agregar `transpose`/`gain` no puede cambiar
    /// un byte de un proyecto que no los usa.
    #[test]
    fn los_defaults_no_se_escriben() {
        let t = instruments_to_toml(&[InstrumentSub::new(1, "a.sf2", 0, 0)]);
        // Por LÍNEA y no por subcadena: buscar "name" suelto matchea
        // «basename» del comentario de cabecera, y el test pasaría a medir el
        // texto de la documentación en vez del formato.
        let campo = |k: &str| t.lines().any(|l| l.starts_with(&format!("{k} = ")));
        assert!(!campo("transpose"), "transpose 0 no se escribe");
        assert!(!campo("gain"), "gain 1.0 no se escribe");
        assert!(!campo("name"), "nombre vacío no se escribe");
        assert!(
            campo("patch") && campo("soundfont") && campo("preset"),
            "lo obligatorio sí se escribe"
        );
        // Y aun así vuelve con los defaults correctos.
        let back = instruments_from_toml(&t);
        assert_eq!(back.len(), 1);
        assert_eq!(back[0].transpose, 0);
        assert_eq!(back[0].gain, 1.0);
    }

    /// Una entrada rota no puede llevarse puesto el resto del mapeo: si un
    /// timbre pierde su asignación, el juego suena con su chip original — que es
    /// una degradación aceptable. Perder las otras 29 no lo sería.
    #[test]
    fn una_entrada_rota_no_tira_las_demas() {
        let t = r#"
[[instrument]]
patch = "0x0000000000000001"
soundfont = "a.sf2"
preset = 10

[[instrument]]
soundfont = "b.sf2"
preset = 11

[[instrument]]
patch = "0x0000000000000003"
preset = 12

[[instrument]]
patch = "0x0000000000000004"
soundfont = "d.sf2"

[[instrument]]
patch = "0x0000000000000005"
soundfont = "e.sf2"
preset = 15
"#;
        let back = instruments_from_toml(t);
        assert_eq!(
            back.len(),
            2,
            "entran la primera y la última; las tres del medio están incompletas"
        );
        assert_eq!(back[0].patch, 1);
        assert_eq!(back[1].patch, 5);
    }

    /// Un TOML basura devuelve vacío en vez de romper.
    #[test]
    fn un_toml_invalido_no_rompe() {
        assert!(instruments_from_toml("esto no es toml [[[").is_empty());
        assert!(instruments_from_toml("").is_empty());
        assert!(instruments_from_toml("[[otra_cosa]]\nx = 1").is_empty());
    }

    /// El agrupado por SoundFont es lo que el horneado consume: un recorte por
    /// archivo de origen, con los presets que le tocan.
    #[test]
    fn agrupa_por_soundfont_sin_repetir() {
        let subs = vec![
            InstrumentSub::new(1, "gm.sf2", 0, 33),
            InstrumentSub::new(2, "cave.sf2", 0, 80),
            InstrumentSub::new(3, "gm.sf2", 0, 33), // mismo preset que el 1
            InstrumentSub::new(4, "gm.sf2", 0, 38),
        ];
        let g = soundfonts_used(&subs);
        assert_eq!(g.len(), 2, "dos archivos de origen");
        assert_eq!(g[0].0, "cave.sf2");
        assert_eq!(g[0].1, vec![(0, 80)]);
        assert_eq!(g[1].0, "gm.sf2");
        assert_eq!(
            g[1].1,
            vec![(0, 33), (0, 38)],
            "sin repetir el preset compartido"
        );
    }

    /// Valores fuera de rango se acotan en vez de propagarse: un `preset` de 999
    /// no existe en SF2, y un `gain` de 1e9 reventaría la mezcla.
    #[test]
    fn los_valores_absurdos_se_acotan_o_descartan() {
        let t = r#"
[[instrument]]
patch = "0x1"
soundfont = "a.sf2"
preset = 999

[[instrument]]
patch = "0x2"
soundfont = "a.sf2"
preset = 5
gain = 1000.0
transpose = 900
"#;
        let back = instruments_from_toml(t);
        assert_eq!(
            back.len(),
            1,
            "el preset fuera de rango descarta la entrada"
        );
        assert_eq!(back[0].gain, 8.0, "la ganancia se acota");
        assert_eq!(back[0].transpose, 48, "la transposición se acota");
    }
}
