//! Conversion of Ogg Vorbis-compressed SF3 files into plain SF2 data.
//!
//! Conversion happens at the ingestion boundary so synthesis and pack baking
//! operate on one normalized SoundFont representation.

// SF3 → SF2: descomprimir un SoundFont con samples Ogg Vorbis.
//
// QUÉ ES UN SF3. El formato que popularizó MuseScore/FluidSynth: la MISMA
// estructura RIFF/sfbk del SF2, pero cada sample del chunk `smpl` viaja como
// un stream Ogg Vorbis independiente. Lo delatan dos señales: `ifil` con
// versión mayor ≥ 3 y el bit 0x10 (`CompressedInVorbis`) en el `sampleType`
// del shdr. Para un sample comprimido, `start`/`end` son offsets en BYTES
// dentro de `smpl` (no índices de muestra) y los loops son RELATIVOS al
// comienzo del sample decodificado.
//
// POR QUÉ CONVERTIR Y NO ENSEÑARLE SF3 A RustySynth. Todo el pipeline aguas
// abajo —el sintetizador, el horneador recortado (`sf2_bake`), el pack— habla
// SF2 plano, y el pack DEBE seguir llevando SF2 plano: el runtime no puede
// pagar un decode Vorbis al abrir cada pack ni cargar un formato más. La
// conversión pasa una sola vez en la frontera (al listar/cargar/hornear) y
// el resto del sistema no se entera.
//
// El pdta se copia sub-chunk por sub-chunk TAL CUAL (mismo criterio que el
// horneador: re-emitir desde valores interpretados sería lossy) — sólo el
// shdr se reescribe, porque los offsets cambian de bytes-comprimidos a
// índices de muestra.

use std::io::Cursor;

/// El spec exige 46 muestras de silencio después de cada sample.
const SAMPLE_PAD: usize = 46;
/// Bit de `sampleType`: el sample está comprimido en Vorbis (convención SF3).
const TY_VORBIS: u16 = 0x10;

fn rd16(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}
fn rd32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

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

/// Recorre los sub-chunks de un LIST: (id, cuerpo).
fn walk_list(b: &[u8]) -> Vec<(&[u8], &[u8])> {
    let mut out = Vec::new();
    let mut at = 4; // saltar el form-type
    while at + 8 <= b.len() {
        let id = &b[at..at + 4];
        let sz = rd32(b, at + 4) as usize;
        let body_at = at + 8;
        if body_at + sz > b.len() {
            break;
        }
        out.push((id, &b[body_at..body_at + sz]));
        at = body_at + sz + (sz & 1);
    }
    out
}

/// Returns whether the SoundFont advertises version 3 or contains a
/// Vorbis-compressed sample.
pub fn is_sf3(src: &[u8]) -> bool {
    if src.len() < 12 || &src[0..4] != b"RIFF" || &src[8..12] != b"sfbk" {
        return false;
    }
    let mut at = 12;
    while at + 8 <= src.len() {
        let id = &src[at..at + 4];
        let sz = rd32(src, at + 4) as usize;
        let body_at = at + 8;
        if body_at + sz > src.len() {
            break;
        }
        let body = &src[body_at..body_at + sz];
        if id == b"LIST" && sz >= 4 {
            match &body[0..4] {
                b"INFO" => {
                    for (cid, c) in walk_list(body) {
                        if cid == b"ifil" && c.len() >= 2 && rd16(c, 0) >= 3 {
                            return true;
                        }
                    }
                }
                b"pdta" => {
                    for (cid, c) in walk_list(body) {
                        if cid == b"shdr" {
                            for r in c.chunks_exact(46) {
                                if rd16(r, 44) & TY_VORBIS != 0 {
                                    return true;
                                }
                            }
                        }
                    }
                }
                _ => {}
            }
        }
        at = body_at + sz + (sz & 1);
    }
    false
}

/// Decodifica UN stream Ogg Vorbis a PCM i16 mono.
///
/// Los samples de un SoundFont son mono por definición (el estéreo son dos
/// samples enlazados por `link`); si un archivo raro trae un Ogg estéreo, se
/// queda el canal izquierdo — promediar cambiaría la fase contra su pareja.
fn decode_ogg_mono(bytes: &[u8]) -> Result<Vec<i16>, String> {
    let mut rdr = lewton::inside_ogg::OggStreamReader::new(Cursor::new(bytes))
        .map_err(|e| format!("Ogg: {e:?}"))?;
    let ch = rdr.ident_hdr.audio_channels as usize;
    if ch == 0 {
        return Err("Ogg sin canales".into());
    }
    let mut pcm: Vec<i16> = Vec::new();
    while let Some(pkt) = rdr
        .read_dec_packet_itl()
        .map_err(|e| format!("Vorbis: {e:?}"))?
    {
        if ch == 1 {
            pcm.extend_from_slice(&pkt);
        } else {
            pcm.extend(pkt.chunks_exact(ch).map(|f| f[0]));
        }
    }
    Ok(pcm)
}

/// Converts SF3 bytes to plain SF2 data.
///
/// Returns `Ok(None)` when the source is already SF2. A compressed sample that
/// cannot be decoded is replaced with silence while other samples are retained.
///
/// # Errors
///
/// Returns an error when the RIFF/SoundFont structure is malformed.
pub fn to_sf2(src: &[u8]) -> Result<Option<Vec<u8>>, String> {
    if !is_sf3(src) {
        return Ok(None);
    }

    // -- levantar los tres LIST -------------------------------------------
    let mut smpl: &[u8] = &[];
    let mut pdta: Option<&[u8]> = None;
    let mut at = 12;
    while at + 8 <= src.len() {
        let id = &src[at..at + 4];
        let sz = rd32(src, at + 4) as usize;
        let body_at = at + 8;
        if body_at + sz > src.len() {
            break;
        }
        let body = &src[body_at..body_at + sz];
        if id == b"LIST" && sz >= 4 {
            match &body[0..4] {
                b"sdta" => {
                    for (cid, c) in walk_list(body) {
                        if cid == b"smpl" {
                            smpl = c;
                        }
                    }
                }
                b"pdta" => pdta = Some(body),
                _ => {}
            }
        }
        at = body_at + sz + (sz & 1);
    }
    let pdta = pdta.ok_or("SF3 sin chunk pdta")?;

    let shdr_in = walk_list(pdta)
        .into_iter()
        .find(|(id, _)| *id == b"shdr")
        .map(|(_, b)| b)
        .ok_or("SF3 sin shdr")?;

    // -- reconstruir smpl + shdr ------------------------------------------
    // Vista i16 del smpl viejo, para los samples que NO están comprimidos
    // (un SF3 puede mezclar): para ésos start/end siguen siendo índices.
    let smpl_i16: Vec<i16> = smpl
        .chunks_exact(2)
        .map(|c| i16::from_le_bytes([c[0], c[1]]))
        .collect();

    let mut out_pcm: Vec<i16> = Vec::new();
    let mut shdr_out: Vec<u8> = Vec::new();
    let n_rec = shdr_in.len() / 46;
    for (i, r) in shdr_in.chunks_exact(46).enumerate() {
        if i + 1 == n_rec {
            // El terminal "EOS" se copia tal cual (todo ceros salvo el nombre).
            shdr_out.extend_from_slice(r);
            break;
        }
        let start = rd32(r, 20) as usize;
        let end = rd32(r, 24) as usize;
        let start_loop = rd32(r, 28);
        let end_loop = rd32(r, 32);
        let ty = rd16(r, 44);

        let (pcm, rel_loop): (Vec<i16>, (u32, u32)) = if ty & TY_VORBIS != 0 {
            // start/end en BYTES dentro de smpl; loops RELATIVOS al sample.
            let ogg = if end <= smpl.len() && start < end {
                &smpl[start..end]
            } else {
                &[][..]
            };
            match decode_ogg_mono(ogg) {
                Ok(p) => (p, (start_loop, end_loop)),
                Err(e) => {
                    eprintln!("[sf3] sample {i}: {e} — queda mudo");
                    // Mudo pero NO vacío: un sample de largo 0 no pasa la
                    // validación del sintetizador y tiraría el archivo entero.
                    (vec![0i16; 8], (0, 0))
                }
            }
        } else {
            // Sample plano: índices de muestra, loops ABSOLUTOS en smpl.
            let a = start.min(smpl_i16.len());
            let b = end.min(smpl_i16.len()).max(a);
            (
                smpl_i16[a..b].to_vec(),
                (
                    start_loop.saturating_sub(start as u32),
                    end_loop.saturating_sub(start as u32),
                ),
            )
        };

        let n = pcm.len() as u32;
        let new_start = out_pcm.len() as u32;
        out_pcm.extend_from_slice(&pcm);
        let new_end = out_pcm.len() as u32;
        out_pcm.extend(std::iter::repeat_n(0i16, SAMPLE_PAD));

        shdr_out.extend_from_slice(&r[0..20]); // nombre
        shdr_out.extend_from_slice(&new_start.to_le_bytes());
        shdr_out.extend_from_slice(&new_end.to_le_bytes());
        // Loops re-basados y ACOTADOS al sample: hay escritores con loops
        // fuera de rango y un end_loop pasado el final revienta el playback.
        shdr_out.extend_from_slice(&(new_start + rel_loop.0.min(n)).to_le_bytes());
        shdr_out.extend_from_slice(&(new_start + rel_loop.1.min(n)).to_le_bytes());
        shdr_out.extend_from_slice(&r[36..44]); // rate, pitch, corr, link
        shdr_out.extend_from_slice(&(ty & !TY_VORBIS).to_le_bytes());
    }

    // -- emitir ------------------------------------------------------------
    let mut info = Vec::new();
    info.extend_from_slice(b"INFO");
    info.extend(chunk(b"ifil", &[2, 0, 1, 0])); // ya no es v3
    info.extend(chunk(b"isng", b"EMU8000\0"));
    // Tamaño PAR sin depender del padding RIFF: RustySynth no saltea el byte
    // de relleno dentro de INFO y un INAM impar corre la lectura ('?LIS').
    info.extend(chunk(b"INAM", b"Ayther SF3\0\0"));

    let mut smpl_bytes = Vec::with_capacity(out_pcm.len() * 2);
    for v in &out_pcm {
        smpl_bytes.extend_from_slice(&v.to_le_bytes());
    }
    let mut sdta = Vec::new();
    sdta.extend_from_slice(b"sdta");
    sdta.extend(chunk(b"smpl", &smpl_bytes));

    let mut pdta_out = Vec::new();
    pdta_out.extend_from_slice(b"pdta");
    for (id, b) in walk_list(pdta) {
        let id4: &[u8; 4] = id.try_into().map_err(|_| "chunk id corto")?;
        if id == b"shdr" {
            pdta_out.extend(chunk(id4, &shdr_out));
        } else {
            pdta_out.extend(chunk(id4, b));
        }
    }

    let mut body = Vec::new();
    body.extend_from_slice(b"sfbk");
    body.extend(chunk(b"LIST", &info));
    body.extend(chunk(b"LIST", &sdta));
    body.extend(chunk(b"LIST", &pdta_out));
    Ok(Some(chunk(b"RIFF", &body)))
}

/// Normalizes SF3 to SF2 and borrows already-normalized SF2 input.
///
/// Conversion failures fall back to the original bytes so the downstream SF2
/// parser can report its standard error.
pub fn ensure_sf2(src: &[u8]) -> std::borrow::Cow<'_, [u8]> {
    match to_sf2(src) {
        Ok(Some(v)) => std::borrow::Cow::Owned(v),
        Ok(None) => std::borrow::Cow::Borrowed(src),
        Err(e) => {
            eprintln!("[sf3] conversión falló: {e}");
            std::borrow::Cow::Borrowed(src)
        }
    }
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sf2::Sf2Synth;

    /// Un «SF3» estructural con el sample SIN comprimir: versión 3 en ifil pero
    /// sampleType plano. Sirve para probar la detección por versión y que la
    /// conversión conserva un archivo tocable. No se puede fabricar un Vorbis
    /// de verdad acá — no hay encoder en las deps, y no debería haberlo: el
    /// camino de decode real se cubre con AYTHER_SF3_TEST.
    fn structural_sf3() -> Vec<u8> {
        // Arranca del SF2 mínimo compartido del módulo sf2 y le sube la
        // versión — la estructura RIFF no se duplica acá.
        let mut v = crate::sf2::minimal_sf2();
        // Buscar el chunk ifil y pisarle la versión mayor a 3.
        let pos = v.windows(4).position(|w| w == b"ifil").expect("ifil");
        v[pos + 8] = 3;
        v[pos + 9] = 0;
        v
    }

    #[test]
    fn plain_sf2_is_not_sf3() {
        let sf2 = crate::sf2::minimal_sf2();
        assert!(!is_sf3(&sf2));
        assert!(to_sf2(&sf2).unwrap().is_none(), "SF2 plano = passthrough");
    }

    #[test]
    fn sf3_is_detected_and_converted() {
        let sf3 = structural_sf3();
        assert!(is_sf3(&sf3), "ifil 3.x tiene que detectarse");
        let out = to_sf2(&sf3).unwrap().expect("convierte");
        assert!(!is_sf3(&out), "el resultado ya no es SF3");

        // EL oráculo de la casa: que el convertido CARGUE y SUENE.
        let mut s = Sf2Synth::new(&out, 48000).expect("el convertido carga");
        s.note_on(0, 60, 100);
        let mut buf = vec![0.0f32; 4096];
        s.render_interleaved(&mut buf);
        let peak = buf.iter().fold(0.0f32, |a, &b| a.max(b.abs()));
        assert!(
            peak > 1e-3,
            "el SF3 convertido tiene que sonar, pico {peak}"
        );
    }

    #[test]
    fn broken_ogg_mutes_sample_without_aborting() {
        let mut sf3 = structural_sf3();
        // Marcar el sample como comprimido: sus bytes de PCM no son un Ogg
        // válido, así que el decode falla — el archivo tiene que salir igual.
        let pos = sf3.windows(4).position(|w| w == b"shdr").expect("shdr");
        // sampleType del primer registro: 8 (header) + 44 de offset.
        let ty_at = pos + 8 + 44;
        let ty = u16::from_le_bytes([sf3[ty_at], sf3[ty_at + 1]]) | TY_VORBIS;
        sf3[ty_at..ty_at + 2].copy_from_slice(&ty.to_le_bytes());

        let out = to_sf2(&sf3).unwrap().expect("convierte igual");
        assert!(
            Sf2Synth::new(&out, 48000).is_ok(),
            "el archivo con un sample irrecuperable sigue cargando"
        );
    }

    /// Contra un SF3 REAL (MuseScore u otro), activado por env — mismo criterio
    /// que `bakes_real_sf2`: un test no puede depender de la colección de
    /// nadie. Verifica el decode Vorbis de verdad y que el resultado suena.
    #[test]
    fn converts_real_sf3() {
        let Ok(path) = std::env::var("AYTHER_SF3_TEST") else {
            eprintln!("[skip] sin AYTHER_SF3_TEST=<ruta a un .sf3>");
            return;
        };
        let src = std::fs::read(&path).expect("leer el SF3");
        assert!(is_sf3(&src), "el archivo de prueba no parece SF3");
        let out = to_sf2(&src).unwrap().expect("convierte");
        eprintln!(
            "[..] SF3 {:.1} MB -> SF2 {:.1} MB",
            src.len() as f64 / 1048576.0,
            out.len() as f64 / 1048576.0
        );

        let presets = crate::sf2_bake::list_presets(&out).expect("listar");
        assert!(!presets.is_empty());
        let mut s = Sf2Synth::new(&out, 48000).expect("carga");
        let mut sono = 0;
        for key in [48, 60, 72] {
            s.all_notes_off();
            let mut warm = vec![0.0f32; 2048];
            s.render_interleaved(&mut warm);
            s.note_on(0, key, 100);
            let mut buf = vec![0.0f32; 8192];
            s.render_interleaved(&mut buf);
            if buf.iter().fold(0.0f32, |a, &b| a.max(b.abs())) > 1e-3 {
                sono += 1;
            }
        }
        assert!(sono > 0, "ninguna nota del SF3 convertido sonó");
    }
}
