//! Conversion of SFZ instruments and referenced samples into SF2 data.
//!
//! The converter implements the playable core of the SFZ format and normalizes
//! supported key, velocity, tuning, envelope, pan, and loop settings.

// SFZ → SF2: un instrumento de texto con samples sueltos, normalizado (
// ampliado).
//
// QUÉ ES UN SFZ. Un archivo de TEXTO (headers `<region>`/`<group>`/… con
// opcodes `clave=valor`) que referencia samples sueltos en disco (WAV/FLAC/
// OGG). Es el formato de facto de los instrumentos gratuitos — pero no hay
// «un SFZ»: el vocabulario de opcodes es enorme y cada biblioteca usa su
// rincón. Acá se implementa el SUBSET que define un instrumento tocable:
// rangos de tecla y velocidad, keycenter, afinación, volumen/pan, loops y la
// envolvente de amplitud. Un opcode desconocido se IGNORA (como hacen todos
// los reproductores SFZ) — el instrumento suena con lo que sí se entiende.
//
// POR QUÉ CONVERTIR A SF2 Y NO SINTETIZAR SFZ. Mismo criterio que sf3.rs:
// todo el pipeline aguas abajo (RustySynth, el horneador recortado, el pack)
// habla SF2 plano y no debe aprender un formato más. Un `.sfz` se normaliza
// UNA vez en la frontera y adentro es un SF2 como cualquier otro: se lista,
// se hornea recortado y viaja al pack igual.
//
// LÍMITES DELIBERADOS del subset, para que nadie los descubra a oído:
// - un `.sfz` = UN preset (banco 0, preset 0, nombre = el archivo);
// - samples estéreo se bajan a mono (promedio): el SF2 estéreo exige pares
//   enlazados y las voces del chip que esto reemplaza son mono de todos modos;
// - filtros (fil_*), LFOs y curvas no se traducen — RustySynth tampoco cubre
//   todo eso; la envolvente de amplitud sí, que es lo que define el ataque.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// El spec SF2 exige 46 muestras de silencio después de cada sample.
const SAMPLE_PAD: usize = 46;

// Generadores SF2 que emite el builder.
const GEN_PAN: u16 = 17;
const GEN_ATTACK: u16 = 34;
const GEN_HOLD: u16 = 35;
const GEN_DECAY: u16 = 36;
const GEN_SUSTAIN: u16 = 37;
const GEN_RELEASE: u16 = 38;
const GEN_INSTRUMENT: u16 = 41;
const GEN_KEY_RANGE: u16 = 43;
const GEN_VEL_RANGE: u16 = 44;
const GEN_ATTENUATION: u16 = 48;
const GEN_COARSE_TUNE: u16 = 51;
const GEN_FINE_TUNE: u16 = 52;
const GEN_SAMPLE_ID: u16 = 53;
const GEN_SAMPLE_MODES: u16 = 54;
const GEN_ROOT_KEY: u16 = 58;

// -- parser ------------------------------------------------------------------

/// Una región ya con su herencia resuelta: global ∪ master ∪ group ∪ region.
type Opcodes = HashMap<String, String>;

/// Lee el texto de un.sfz resolviendo `#include` (relativo al archivo que lo
/// incluye) y `#define $VAR valor`. La recursión se acota: un include circular
/// no debe colgar el Lab.
fn read_text(path: &Path, depth: u32) -> Result<String, String> {
    if depth > 8 {
        return Err(format!("#include demasiado anidado en {}", path.display()));
    }
    let raw = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let text = String::from_utf8_lossy(&raw).into_owned();
    let dir = path.parent().unwrap_or(Path::new("."));

    let mut out = String::with_capacity(text.len());
    for line in text.lines() {
        let t = line.trim_start();
        if let Some(rest) = t.strip_prefix("#include") {
            let inc = rest.trim().trim_matches('"');
            if inc.is_empty() {
                continue;
            }
            out.push_str(&read_text(&dir.join(inc.replace('\\', "/")), depth + 1)?);
            out.push('\n');
        } else {
            out.push_str(line);
            out.push('\n');
        }
    }
    Ok(out)
}

/// Separa una línea en headers `<...>` y pares `clave=valor`. El valor de un
/// opcode puede llevar ESPACIOS (`sample=piano C4 v2.wav`): corre hasta el
/// próximo `palabra=` de la misma línea o el fin de línea — la regla de todos
/// los parsers SFZ, porque el formato no tiene comillas.
fn split_line(line: &str, out: &mut Vec<Tok>) {
    let mut rest = line;
    loop {
        rest = rest.trim_start();
        if rest.is_empty() {
            return;
        }
        if let Some(r) = rest.strip_prefix('<') {
            match r.find('>') {
                Some(e) => {
                    out.push(Tok::Header(r[..e].to_ascii_lowercase()));
                    rest = &r[e + 1..];
                    continue;
                }
                None => return,
            }
        }
        let Some(eq) = rest.find('=') else { return };
        let key = rest[..eq]
            .trim()
            .rsplit(char::is_whitespace)
            .next()
            .unwrap_or("")
            .to_ascii_lowercase();
        let after = &rest[eq + 1..];
        // ¿Hay OTRO opcode en la línea? El valor llega hasta su palabra-clave.
        let next_key_at = after.find('=').map(|e2| {
            after[..e2]
                .rfind(char::is_whitespace)
                .map(|w| w + 1)
                .unwrap_or(0)
        });
        let (val, next) = match next_key_at {
            Some(k) if k > 0 => (&after[..k], &after[k..]),
            _ => (after, ""),
        };
        if !key.is_empty() {
            out.push(Tok::Opcode(key, val.trim().to_string()));
        }
        if next.is_empty() {
            return;
        }
        rest = next;
    }
}

enum Tok {
    Header(String),
    Opcode(String, String),
}

/// Parsea el texto ya expandido a la lista de regiones con herencia resuelta.
/// Devuelve además el `default_path` del `<control>` vigente por región.
fn parse_regions(text: &str) -> Vec<(Opcodes, String)> {
    // #define: se acumulan y se sustituyen textualmente, que es lo que hace
    // el formato (son macros, no variables con scope).
    let mut defines: Vec<(String, String)> = Vec::new();
    let mut toks: Vec<Tok> = Vec::new();
    for raw in text.lines() {
        let line = match raw.find("//") {
            Some(c) => &raw[..c],
            None => raw,
        };
        let t = line.trim_start();
        if let Some(rest) = t.strip_prefix("#define") {
            let mut it = rest.trim().splitn(2, char::is_whitespace);
            if let (Some(k), Some(v)) = (it.next(), it.next())
                && k.starts_with('$')
            {
                defines.push((k.to_string(), v.trim().to_string()));
            }
            continue;
        }
        let mut line = line.to_string();
        // Las macros más largas primero: $KEYLOW no debe pisarse con $KEY.
        let mut ds = defines.clone();
        ds.sort_by_key(|item| std::cmp::Reverse(item.0.len()));
        for (k, v) in &ds {
            line = line.replace(k.as_str(), v);
        }
        split_line(&line, &mut toks);
    }

    let mut control = String::new(); // default_path vigente
    let mut global: Opcodes = HashMap::new();
    let mut master: Opcodes = HashMap::new();
    let mut group: Opcodes = HashMap::new();
    let mut scope = ""; // header actual
    let mut region: Option<Opcodes> = None;
    let mut out: Vec<(Opcodes, String)> = Vec::new();

    let mut flush = |region: &mut Option<Opcodes>, control: &String| {
        if let Some(r) = region.take() {
            out.push((r, control.clone()));
        }
    };

    for tok in toks {
        match tok {
            Tok::Header(h) => {
                flush(&mut region, &control);
                match h.as_str() {
                    "control" => scope = "control",
                    "global" => {
                        scope = "global";
                        global.clear();
                        master.clear();
                        group.clear();
                    }
                    "master" => {
                        scope = "master";
                        master.clear();
                        group.clear();
                    }
                    "group" => {
                        scope = "group";
                        group.clear();
                    }
                    "region" => {
                        scope = "region";
                        let mut r = global.clone();
                        r.extend(master.clone());
                        r.extend(group.clone());
                        region = Some(r);
                    }
                    _ => scope = "", // <curve>/<effect>/…: se ignora entero
                }
            }
            Tok::Opcode(k, v) => match scope {
                "control" if k == "default_path" => {
                    control = v;
                }
                "global" => {
                    global.insert(k, v);
                }
                "master" => {
                    master.insert(k, v);
                }
                "group" => {
                    group.insert(k, v);
                }
                "region" => {
                    if let Some(r) = region.as_mut() {
                        r.insert(k, v);
                    }
                }
                _ => {}
            },
        }
    }
    flush(&mut region, &control);
    out
}

/// Nota MIDI desde un opcode: número (`60`) o nombre (`c4`, `f`, `db5`) —
/// convención SFZ: c-1 = 0, o sea c4 = 60.
fn parse_note(v: &str) -> Option<i32> {
    if let Ok(n) = v.parse::<i32>() {
        return (0..=127).contains(&n).then_some(n);
    }
    let b = v.as_bytes();
    if b.is_empty() {
        return None;
    }
    let sem = match b[0].to_ascii_lowercase() {
        b'c' => 0,
        b'd' => 2,
        b'e' => 4,
        b'f' => 5,
        b'g' => 7,
        b'a' => 9,
        b'b' => 11,
        _ => return None,
    };
    let mut at = 1;
    let mut sem = sem;
    if at < b.len() && (b[at] == b'#' || b[at] == b'b') {
        sem += if b[at] == b'#' { 1 } else { -1 };
        at += 1;
    }
    let oct: i32 = v[at..].parse().ok()?;
    let n = (oct + 1) * 12 + sem;
    (0..=127).contains(&n).then_some(n)
}

fn f(op: &Opcodes, k: &str) -> Option<f64> {
    op.get(k).and_then(|v| v.parse().ok())
}
fn note(op: &Opcodes, k: &str) -> Option<i32> {
    op.get(k).and_then(|v| parse_note(v))
}

/// Segundos → timecents SF2 (gen 34/35/36/38). El piso -12000 (≈1 ms) es el
/// «instantáneo» del formato.
fn timecents(sec: f64) -> i16 {
    if sec <= 0.001 {
        return -12000;
    }
    (1200.0 * sec.log2()).round().clamp(-12000.0, 8000.0) as i16
}

/// ampeg_sustain (%) → atenuación del sustain en centibeles (gen 37).
fn sustain_cb(pct: f64) -> i16 {
    if pct >= 100.0 {
        return 0;
    }
    if pct <= 0.1 {
        return 1440;
    }
    (200.0 * (100.0 / pct).log10()).round().clamp(0.0, 1440.0) as i16
}

// -- samples -----------------------------------------------------------------

/// PCM mono i16 + sample rate de un archivo de sample. El estéreo se
/// promedia — ver los límites del subset en la cabecera del módulo.
fn load_sample(path: &Path) -> Result<(Vec<i16>, u32), String> {
    let ext = path
        .extension()
        .map(|e| e.to_string_lossy().to_ascii_lowercase())
        .unwrap_or_default();
    let err = |e: String| format!("{}: {e}", path.display());
    match ext.as_str() {
        "wav" => {
            let mut r = hound::WavReader::open(path).map_err(|e| err(e.to_string()))?;
            let spec = r.spec();
            let ch = spec.channels.max(1) as usize;
            let mono: Vec<i16> = match spec.sample_format {
                hound::SampleFormat::Float => {
                    let s: Vec<f32> = r
                        .samples::<f32>()
                        .collect::<Result<_, _>>()
                        .map_err(|e| err(e.to_string()))?;
                    s.chunks(ch)
                        .map(|fr| {
                            let m = fr.iter().sum::<f32>() / ch as f32;
                            (m.clamp(-1.0, 1.0) * 32767.0) as i16
                        })
                        .collect()
                }
                hound::SampleFormat::Int => {
                    let shift = spec.bits_per_sample.saturating_sub(16);
                    let s: Vec<i32> = r
                        .samples::<i32>()
                        .collect::<Result<_, _>>()
                        .map_err(|e| err(e.to_string()))?;
                    s.chunks(ch)
                        .map(|fr| {
                            let m = fr.iter().map(|&v| v as i64).sum::<i64>() / ch as i64;
                            if shift > 0 {
                                (m >> shift) as i16
                            } else {
                                (m << (16 - spec.bits_per_sample)) as i16
                            }
                        })
                        .collect()
                }
            };
            Ok((mono, spec.sample_rate))
        }
        "flac" => {
            let mut r = claxon::FlacReader::open(path).map_err(|e| err(e.to_string()))?;
            let si = r.streaminfo();
            let ch = si.channels.max(1) as usize;
            let shift = si.bits_per_sample.saturating_sub(16);
            let s: Vec<i32> = r
                .samples()
                .collect::<Result<_, _>>()
                .map_err(|e| err(e.to_string()))?;
            let mono: Vec<i16> = s
                .chunks(ch)
                .map(|fr| {
                    let m = fr.iter().map(|&v| v as i64).sum::<i64>() / ch as i64;
                    if shift > 0 {
                        (m >> shift) as i16
                    } else {
                        (m << (16 - si.bits_per_sample)) as i16
                    }
                })
                .collect();
            Ok((mono, si.sample_rate))
        }
        "ogg" => {
            let bytes = std::fs::read(path).map_err(|e| err(e.to_string()))?;
            let mut rdr = lewton::inside_ogg::OggStreamReader::new(std::io::Cursor::new(bytes))
                .map_err(|e| err(format!("{e:?}")))?;
            let ch = rdr.ident_hdr.audio_channels.max(1) as usize;
            let rate = rdr.ident_hdr.audio_sample_rate;
            let mut mono: Vec<i16> = Vec::new();
            while let Some(p) = rdr
                .read_dec_packet_itl()
                .map_err(|e| err(format!("{e:?}")))?
            {
                mono.extend(
                    p.chunks(ch)
                        .map(|fr| (fr.iter().map(|&v| v as i32).sum::<i32>() / ch as i32) as i16),
                );
            }
            Ok((mono, rate))
        }
        other => Err(err(format!("extensión de sample no soportada: .{other}"))),
    }
}

// -- builder -----------------------------------------------------------------

fn chunk(id: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(8 + body.len() + 1);
    v.extend_from_slice(id);
    v.extend_from_slice(&(body.len() as u32).to_le_bytes());
    v.extend_from_slice(body);
    if body.len() % 2 == 1 {
        v.push(0);
    }
    v
}
fn put_name(out: &mut Vec<u8>, s: &str) {
    let mut a = [0u8; 20];
    let b = s.as_bytes();
    let n = b.len().min(19);
    a[..n].copy_from_slice(&b[..n]);
    out.extend_from_slice(&a);
}
fn push_generator(out: &mut Vec<u8>, oper: u16, amount: u16) -> u16 {
    out.extend_from_slice(&oper.to_le_bytes());
    out.extend_from_slice(&amount.to_le_bytes());
    1
}

/// Converts an SFZ file and its referenced samples into an in-memory SF2.
///
/// Unreadable sample regions are skipped and reported to stderr. Conversion
/// succeeds when at least one playable region remains.
///
/// # Errors
///
/// Returns an error for invalid SFZ input or when no playable region can be
/// produced.
pub fn to_sf2(sfz_path: &Path) -> Result<Vec<u8>, String> {
    let text = read_text(sfz_path, 0)?;
    let regions = parse_regions(&text);
    if regions.is_empty() {
        return Err(format!("{}: sin regiones <region>", sfz_path.display()));
    }
    let dir = sfz_path.parent().unwrap_or(Path::new("."));
    let stem = sfz_path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "SFZ".into());

    // -- cargar samples (dedup por path + loop) ----------------------------
    struct Smp {
        pcm: Vec<i16>,
        rate: u32,
        lo: u32,
        hi: u32,
        looped: bool,
    }
    let mut samples: Vec<Smp> = Vec::new();
    let mut smp_ix: HashMap<(PathBuf, u32, u32), usize> = HashMap::new();
    struct Zone {
        op: Opcodes,
        smp: usize,
    }
    let mut zones: Vec<Zone> = Vec::new();

    for (op, default_path) in regions {
        let Some(sample) = op.get("sample") else {
            continue;
        };
        let mut rel = default_path.replace('\\', "/");
        if !rel.is_empty() && !rel.ends_with('/') {
            rel.push('/');
        }
        rel.push_str(&sample.replace('\\', "/"));
        let path = dir.join(rel);
        let loop_start = f(&op, "loop_start")
            .or_else(|| f(&op, "loopstart"))
            .unwrap_or(0.0) as u32;
        // SFZ: loop_end INCLUSIVE; SF2: exclusivo — el +1 va acá, una vez.
        let loop_end_in = f(&op, "loop_end").or_else(|| f(&op, "loopend"));
        // u32::MAX = «sin loop»: el cast de -1.0 daría 0 y chocaría con un
        // loop_end=0 legítimo en la clave de dedup.
        let key = (
            path.clone(),
            loop_start,
            loop_end_in.map(|v| v as u32).unwrap_or(u32::MAX),
        );
        let smp = match smp_ix.get(&key) {
            Some(&i) => i,
            None => {
                let (pcm, rate) = match load_sample(&path) {
                    Ok(v) => v,
                    Err(e) => {
                        eprintln!("[sfz] región salteada: {e}");
                        continue;
                    }
                };
                let n = pcm.len() as u32;
                let looped = loop_end_in.is_some()
                    || op
                        .get("loop_mode")
                        .map(|m| m.starts_with("loop_"))
                        .unwrap_or(false);
                let hi = loop_end_in.map(|e| (e as u32 + 1).min(n)).unwrap_or(n);
                samples.push(Smp {
                    pcm,
                    rate,
                    lo: loop_start.min(n),
                    hi,
                    looped,
                });
                smp_ix.insert(key, samples.len() - 1);
                samples.len() - 1
            }
        };
        zones.push(Zone { op, smp });
    }
    if zones.is_empty() {
        return Err(format!(
            "{}: ninguna región con sample legible",
            sfz_path.display()
        ));
    }

    // -- smpl + shdr -------------------------------------------------------
    let mut pcm_all: Vec<i16> = Vec::new();
    let mut shdr = Vec::new();
    let mut bases: Vec<u32> = Vec::new();
    for (i, s) in samples.iter().enumerate() {
        let base = pcm_all.len() as u32;
        bases.push(base);
        pcm_all.extend_from_slice(&s.pcm);
        let end = pcm_all.len() as u32;
        pcm_all.extend(std::iter::repeat_n(0i16, SAMPLE_PAD));
        put_name(&mut shdr, &format!("smp{i}"));
        shdr.extend_from_slice(&base.to_le_bytes());
        shdr.extend_from_slice(&end.to_le_bytes());
        shdr.extend_from_slice(&(base + s.lo).to_le_bytes());
        shdr.extend_from_slice(&(base + s.hi).to_le_bytes());
        shdr.extend_from_slice(&s.rate.to_le_bytes());
        shdr.push(60); // el root real va por gen 58
        shdr.push(0);
        shdr.extend_from_slice(&0u16.to_le_bytes()); // sin link (mono)
        shdr.extend_from_slice(&1u16.to_le_bytes()); // monoSample
    }
    put_name(&mut shdr, "EOS");
    shdr.extend_from_slice(&[0u8; 26]);

    // -- instrumento: una zona por región ----------------------------------
    let mut inst = Vec::new();
    let mut ibag = Vec::new();
    let mut igen = Vec::new();
    let mut igen_n: u16 = 0;
    let mut ibag_n: u16 = 0;
    put_name(&mut inst, &stem);
    inst.extend_from_slice(&0u16.to_le_bytes());
    for z in &zones {
        let op = &z.op;
        ibag.extend_from_slice(&igen_n.to_le_bytes());
        ibag.extend_from_slice(&0u16.to_le_bytes());
        ibag_n += 1;

        // El spec ordena: keyRange PRIMERO, velRange después, sampleID ÚLTIMO.
        let key_one = note(op, "key");
        let lo = note(op, "lokey").or(key_one).unwrap_or(0) as u16;
        let hi = note(op, "hikey").or(key_one).unwrap_or(127) as u16;
        igen_n += push_generator(&mut igen, GEN_KEY_RANGE, lo | (hi << 8));
        let lov = f(op, "lovel").unwrap_or(0.0).clamp(0.0, 127.0) as u16;
        let hiv = f(op, "hivel").unwrap_or(127.0).clamp(0.0, 127.0) as u16;
        igen_n += push_generator(&mut igen, GEN_VEL_RANGE, lov | (hiv << 8));

        let root = note(op, "pitch_keycenter").or(key_one).unwrap_or(60);
        igen_n += push_generator(&mut igen, GEN_ROOT_KEY, root as u16);
        if let Some(t) = f(op, "tune") {
            igen_n += push_generator(
                &mut igen,
                GEN_FINE_TUNE,
                (t.clamp(-99.0, 99.0) as i16) as u16,
            );
        }
        if let Some(t) = f(op, "transpose") {
            igen_n += push_generator(
                &mut igen,
                GEN_COARSE_TUNE,
                (t.clamp(-120.0, 120.0) as i16) as u16,
            );
        }
        if let Some(v) = f(op, "volume") {
            // SF2 sólo ATENÚA (gen 48, cB): volume positivo se queda en 0 —
            // el realce por encima de 1.0 ya lo maneja el motor.
            igen_n += push_generator(
                &mut igen,
                GEN_ATTENUATION,
                ((-v).clamp(0.0, 144.0) * 10.0).round() as u16,
            );
        }
        if let Some(p) = f(op, "pan") {
            igen_n += push_generator(
                &mut igen,
                GEN_PAN,
                ((p.clamp(-100.0, 100.0) * 5.0) as i16) as u16,
            );
        }
        if let Some(a) = f(op, "ampeg_attack") {
            igen_n += push_generator(&mut igen, GEN_ATTACK, timecents(a) as u16);
        }
        if let Some(h) = f(op, "ampeg_hold") {
            igen_n += push_generator(&mut igen, GEN_HOLD, timecents(h) as u16);
        }
        if let Some(d) = f(op, "ampeg_decay") {
            igen_n += push_generator(&mut igen, GEN_DECAY, timecents(d) as u16);
        }
        if let Some(s) = f(op, "ampeg_sustain") {
            igen_n += push_generator(&mut igen, GEN_SUSTAIN, sustain_cb(s) as u16);
        }
        if let Some(r) = f(op, "ampeg_release") {
            igen_n += push_generator(&mut igen, GEN_RELEASE, timecents(r) as u16);
        }
        // loop_mode manda; sin él, tener loop_end definido implica loop
        // continuo (la convención de los reproductores SFZ).
        let modes = match op.get("loop_mode").map(String::as_str) {
            Some("loop_continuous") => 1u16,
            Some("loop_sustain") => 3,
            Some(_) => 0, // no_loop / one_shot
            None => {
                if samples[z.smp].looped {
                    1
                } else {
                    0
                }
            }
        };
        if modes != 0 {
            igen_n += push_generator(&mut igen, GEN_SAMPLE_MODES, modes);
        }
        igen_n += push_generator(&mut igen, GEN_SAMPLE_ID, z.smp as u16);
    }
    put_name(&mut inst, "EOI");
    inst.extend_from_slice(&ibag_n.to_le_bytes());
    ibag.extend_from_slice(&igen_n.to_le_bytes());
    ibag.extend_from_slice(&0u16.to_le_bytes());
    igen.extend_from_slice(&[0u8; 4]);

    // -- preset único (banco 0, preset 0) ----------------------------------
    let mut phdr = Vec::new();
    put_name(&mut phdr, &stem);
    phdr.extend_from_slice(&0u16.to_le_bytes()); // preset
    phdr.extend_from_slice(&0u16.to_le_bytes()); // bank
    phdr.extend_from_slice(&0u16.to_le_bytes()); // bag
    phdr.extend_from_slice(&[0u8; 12]);
    put_name(&mut phdr, "EOP");
    phdr.extend_from_slice(&[0u8; 4]);
    phdr.extend_from_slice(&1u16.to_le_bytes());
    phdr.extend_from_slice(&[0u8; 12]);
    let mut pbag = Vec::new();
    pbag.extend_from_slice(&0u16.to_le_bytes());
    pbag.extend_from_slice(&0u16.to_le_bytes());
    pbag.extend_from_slice(&1u16.to_le_bytes());
    pbag.extend_from_slice(&0u16.to_le_bytes());
    let mut pgen = Vec::new();
    push_generator(&mut pgen, GEN_INSTRUMENT, 0);
    pgen.extend_from_slice(&[0u8; 4]);

    // -- armar el archivo --------------------------------------------------
    let mut info = Vec::new();
    info.extend_from_slice(b"INFO");
    info.extend(chunk(b"ifil", &[2, 0, 1, 0]));
    info.extend(chunk(b"isng", b"EMU8000\0"));
    let mut nm = stem.as_bytes().to_vec();
    nm.push(0);
    if nm.len() % 2 == 1 {
        nm.push(0);
    }
    info.extend(chunk(b"INAM", &nm));

    let mut smpl_bytes = Vec::with_capacity(pcm_all.len() * 2);
    for v in &pcm_all {
        smpl_bytes.extend_from_slice(&v.to_le_bytes());
    }
    let mut sdta = Vec::new();
    sdta.extend_from_slice(b"sdta");
    sdta.extend(chunk(b"smpl", &smpl_bytes));

    let mut pdta = Vec::new();
    pdta.extend_from_slice(b"pdta");
    pdta.extend(chunk(b"phdr", &phdr));
    pdta.extend(chunk(b"pbag", &pbag));
    pdta.extend(chunk(b"pmod", &[0u8; 10]));
    pdta.extend(chunk(b"pgen", &pgen));
    pdta.extend(chunk(b"inst", &inst));
    pdta.extend(chunk(b"ibag", &ibag));
    pdta.extend(chunk(b"imod", &[0u8; 10]));
    pdta.extend(chunk(b"igen", &igen));
    pdta.extend(chunk(b"shdr", &shdr));

    let mut body = Vec::new();
    body.extend_from_slice(b"sfbk");
    body.extend(chunk(b"LIST", &info));
    body.extend(chunk(b"LIST", &sdta));
    body.extend(chunk(b"LIST", &pdta));
    Ok(chunk(b"RIFF", &body))
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sf2::Sf2Synth;

    fn write_wav(path: &Path, frames: u32, freq: f32) {
        let spec = hound::WavSpec {
            channels: 1,
            sample_rate: 44100,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        };
        let mut w = hound::WavWriter::create(path, spec).unwrap();
        for i in 0..frames {
            let t = i as f32 / 44100.0;
            w.write_sample(((t * freq * std::f32::consts::TAU).sin() * 16000.0) as i16)
                .unwrap();
        }
        w.finalize().unwrap();
    }

    fn tmpdir(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("ayther_sfz_{name}"));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    /// EL oráculo de la casa: que el convertido CARGUE y SUENE — y que el
    /// rango de teclas se respete (nota fuera de rango = silencio), que es
    /// donde un builder con los generadores mal ordenados falla SIN error.
    #[test]
    fn minimal_sfz_renders_and_respects_ranges() {
        let d = tmpdir("min");
        write_wav(&d.join("tono.wav"), 22050, 440.0);
        std::fs::write(
            d.join("inst.sfz"),
            "// instrumento de prueba\n\
             <group> ampeg_release=0.2\n\
             <region> sample=tono.wav lokey=48 hikey=72 pitch_keycenter=60\n",
        )
        .unwrap();

        let sf2 = to_sf2(&d.join("inst.sfz")).expect("convierte");
        let mut s = Sf2Synth::new(&sf2, 48000).expect("carga");
        assert_eq!(s.presets().len(), 1, "un .sfz = un preset");

        let peak_of = |s: &mut Sf2Synth, key: i32| {
            s.all_notes_off();
            let mut warm = vec![0.0f32; 8192];
            s.render_interleaved(&mut warm);
            s.note_on(0, key, 100);
            let mut buf = vec![0.0f32; 8192];
            s.render_interleaved(&mut buf);
            buf.iter().fold(0.0f32, |a, &b| a.max(b.abs()))
        };
        assert!(
            peak_of(&mut s, 60) > 1e-3,
            "la nota en rango tiene que sonar"
        );
        assert!(
            peak_of(&mut s, 30) < 1e-4,
            "una nota FUERA del keyRange tiene que callar"
        );
        let _ = std::fs::remove_dir_all(&d);
    }

    /// Valores con espacios y default_path: el modo de fallo silencioso del
    /// parser (cortar `sample=` en el primer espacio) rompería justo las
    /// bibliotecas reales, que nombran samples como «Piano C4 v2.wav».
    #[test]
    fn sample_with_spaces_and_default_path() {
        let d = tmpdir("esp");
        std::fs::create_dir_all(d.join("wavs")).unwrap();
        write_wav(&d.join("wavs/piano C4 v2.wav"), 8192, 261.6);
        std::fs::write(
            d.join("inst.sfz"),
            "<control> default_path=wavs/\n\
             <region> sample=piano C4 v2.wav key=c4\n",
        )
        .unwrap();
        let sf2 = to_sf2(&d.join("inst.sfz")).expect("convierte");
        let mut s = Sf2Synth::new(&sf2, 48000).expect("carga");
        s.note_on(0, 60, 100);
        let mut buf = vec![0.0f32; 8192];
        s.render_interleaved(&mut buf);
        assert!(buf.iter().fold(0.0f32, |a, &b| a.max(b.abs())) > 1e-3);
        let _ = std::fs::remove_dir_all(&d);
    }

    /// Herencia group→region + #define + #include: lo que usan las bibliotecas
    /// grandes. El include es textual y el define es macro — si cualquiera
    /// falla, regiones enteras desaparecen sin error.
    #[test]
    fn include_define_and_inheritance() {
        let d = tmpdir("inc");
        write_wav(&d.join("a.wav"), 4096, 440.0);
        write_wav(&d.join("b.wav"), 4096, 880.0);
        std::fs::write(
            d.join("regiones.sfz"),
            "<region> sample=a.wav lokey=$LO hikey=59\n\
             <region> sample=b.wav lokey=60 hikey=$HI\n",
        )
        .unwrap();
        std::fs::write(
            d.join("inst.sfz"),
            "#define $LO 0\n#define $HI 127\n\
             <group> ampeg_attack=0.01\n\
             #include \"regiones.sfz\"\n",
        )
        .unwrap();
        let sf2 = to_sf2(&d.join("inst.sfz")).expect("convierte");
        let mut s = Sf2Synth::new(&sf2, 48000).expect("carga");
        // Las DOS mitades del teclado tienen que sonar (una región cada una).
        for key in [40, 80] {
            s.all_notes_off();
            let mut warm = vec![0.0f32; 8192];
            s.render_interleaved(&mut warm);
            s.note_on(0, key, 100);
            let mut buf = vec![0.0f32; 8192];
            s.render_interleaved(&mut buf);
            let peak = buf.iter().fold(0.0f32, |a, &b| a.max(b.abs()));
            assert!(peak > 1e-3, "la nota {key} tiene que sonar, pico {peak}");
        }
        let _ = std::fs::remove_dir_all(&d);
    }

    /// loop_continuous tiene que SEGUIR sonando pasada la duración del sample;
    /// sin loop tiene que agotarse. Es el observable de que loop_start/end
    /// llegaron al shdr y sampleModes a la zona.
    #[test]
    fn loop_is_preserved() {
        let d = tmpdir("loop");
        write_wav(&d.join("corto.wav"), 4410, 440.0); // 0,1 s
        std::fs::write(
            d.join("loop.sfz"),
            "<region> sample=corto.wav key=60 loop_mode=loop_continuous \
             loop_start=100 loop_end=4000\n",
        )
        .unwrap();
        std::fs::write(d.join("dry.sfz"), "<region> sample=corto.wav key=60\n").unwrap();

        let tail_peak = |sfz: &Path| {
            let sf2 = to_sf2(sfz).expect("convierte");
            let mut s = Sf2Synth::new(&sf2, 44100).expect("carga");
            s.note_on(0, 60, 100);
            // 0,5 s: cinco veces el sample. El loop sigue; el seco se agotó.
            let mut buf = vec![0.0f32; 44100];
            s.render_interleaved(&mut buf);
            buf[buf.len() - 8192..]
                .iter()
                .fold(0.0f32, |a, &b| a.max(b.abs()))
        };
        assert!(
            tail_peak(&d.join("loop.sfz")) > 1e-3,
            "el loop tiene que seguir sonando pasada la duración"
        );
        assert!(
            tail_peak(&d.join("dry.sfz")) < 1e-3,
            "sin loop el sample se agota"
        );
        let _ = std::fs::remove_dir_all(&d);
    }

    /// Una región con el sample ausente se saltea; el resto del instrumento
    /// sobrevive — rescatar lo sano, igual que el horneador.
    #[test]
    fn broken_region_does_not_discard_instrument() {
        let d = tmpdir("rota");
        write_wav(&d.join("ok.wav"), 4096, 440.0);
        std::fs::write(
            d.join("inst.sfz"),
            "<region> sample=no_existe.wav lokey=0 hikey=59\n\
             <region> sample=ok.wav lokey=60 hikey=127\n",
        )
        .unwrap();
        let sf2 = to_sf2(&d.join("inst.sfz")).expect("convierte con lo sano");
        assert!(Sf2Synth::new(&sf2, 48000).is_ok());
        let _ = std::fs::remove_dir_all(&d);

        let d2 = tmpdir("vacia");
        std::fs::write(d2.join("inst.sfz"), "<region> sample=nada.wav\n").unwrap();
        assert!(
            to_sf2(&d2.join("inst.sfz")).is_err(),
            "sin NINGUNA región legible sí falla"
        );
        let _ = std::fs::remove_dir_all(&d2);
    }

    #[test]
    fn notes_by_name() {
        assert_eq!(parse_note("c4"), Some(60));
        assert_eq!(parse_note("C#4"), Some(61));
        assert_eq!(parse_note("db4"), Some(61));
        assert_eq!(parse_note("a0"), Some(21));
        assert_eq!(parse_note("60"), Some(60));
        assert_eq!(parse_note("c-1"), Some(0));
        assert_eq!(parse_note("h4"), None);
        assert_eq!(parse_note("200"), None);
    }
}
