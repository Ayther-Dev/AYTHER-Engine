//! SoundFont baking for distribution-sized `.ay` packs.
//!
//! The baker copies only requested presets, their instruments, and referenced
//! samples while preserving raw SF2 generator values.

// Horneado de SoundFonts: extraer los presets usados y emitir un SF2 mínimo.
//
// POR QUÉ HACE FALTA. Una colección real de SoundFonts pesa cientos de MB —en
// la del usuario hay uno de 988 MB. Desde el pack abre lazy (ya no se
// carga entero a RAM), pero el motivo del recorte sigue en pie: es tamaño de
// DISTRIBUCIÓN — nadie descarga 988 MB de biblioteca para usar 20 timbres.
//
// La regla del pack es la de siempre: viaja lo que se usa. Una banda sonora
// entera necesita del orden de 10-30 timbres, o sea unos pocos MB de muestras
// contra los cientos del archivo original.
//
// POR QUÉ SE PARSEA A MANO Y NO CON RustySynth. La crate expone las regiones ya
// RESUELTAS (`get_pan() -> f32`, etc.), no los generadores crudos — esos son
// `pub(crate)`. Re-emitir desde valores convertidos sería ida y vuelta lossy.
// Acá se leen los registros tal como están y se re-emiten igual: lo único que
// cambia son los índices, que se remapean.
//
// LOS MODULATORS SE DESCARTAN, y no es una simplificación gratuita: RustySynth
// —el sintetizador elegido— los ignora (`soundfont_parameters.rs` hace
// `discard_data` sobre `pmod` e `imod`). Llevarlos sería cargar bytes que
// nadie va a leer.

/// Generadores del spec que este horneador necesita entender. El resto se copia
/// tal cual sin interpretarlo.
const GEN_INSTRUMENT: u16 = 41;
const GEN_SAMPLE_ID: u16 = 53;

/// El spec exige 46 muestras de silencio después de cada sample.
const SAMPLE_PAD: usize = 46;

// -- lectura -----------------------------------------------------------------

fn rd16(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}
fn rd32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}
fn name20(b: &[u8], at: usize) -> String {
    let s = &b[at..at + 20];
    let n = s.iter().position(|&c| c == 0).unwrap_or(20);
    String::from_utf8_lossy(&s[..n]).into_owned()
}

#[derive(Clone)]
struct PresetHdr {
    name: String,
    preset: u16,
    bank: u16,
    bag: u16,
}
#[derive(Clone)]
struct InstHdr {
    name: String,
    bag: u16,
}
#[derive(Clone, Copy)]
struct Bag {
    generator_index: u16,
    _mod: u16,
}
#[derive(Clone, Copy)]
struct Gen {
    oper: u16,
    amount: u16,
}
#[derive(Clone)]
struct Shdr {
    name: String,
    start: u32,
    end: u32,
    start_loop: u32,
    end_loop: u32,
    rate: u32,
    pitch: u8,
    correction: i8,
    link: u16,
    ty: u16,
}

struct Sf2 {
    smpl: Vec<i16>,
    phdr: Vec<PresetHdr>,
    pbag: Vec<Bag>,
    pgen: Vec<Gen>,
    inst: Vec<InstHdr>,
    ibag: Vec<Bag>,
    igen: Vec<Gen>,
    shdr: Vec<Shdr>,
}

/// Recorre los chunks RIFF de un LIST y devuelve (id, cuerpo) de cada uno.
fn walk_list(b: &[u8]) -> Vec<(&[u8], &[u8])> {
    let mut out = Vec::new();
    let mut at = 4; // saltar el form-type ("INFO"/"sdta"/"pdta")
    while at + 8 <= b.len() {
        let id = &b[at..at + 4];
        let sz = rd32(b, at + 4) as usize;
        let body_at = at + 8;
        if body_at + sz > b.len() {
            break;
        }
        out.push((id, &b[body_at..body_at + sz]));
        at = body_at + sz + (sz & 1); // los chunks van a tamaño par
    }
    out
}

fn parse(src: &[u8]) -> Result<Sf2, String> {
    if src.len() < 12 || &src[0..4] != b"RIFF" || &src[8..12] != b"sfbk" {
        return Err("no es un SF2 (falta RIFF/sfbk)".into());
    }
    let mut smpl: Vec<i16> = Vec::new();
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
                    for (cid, cbody) in walk_list(body) {
                        if cid == b"smpl" {
                            smpl = cbody
                                .chunks_exact(2)
                                .map(|c| i16::from_le_bytes([c[0], c[1]]))
                                .collect();
                        }
                    }
                }
                b"pdta" => pdta = Some(body),
                _ => {}
            }
        }
        at = body_at + sz + (sz & 1);
    }

    let pdta = pdta.ok_or("SF2 sin chunk pdta")?;
    let mut s = Sf2 {
        smpl,
        phdr: Vec::new(),
        pbag: Vec::new(),
        pgen: Vec::new(),
        inst: Vec::new(),
        ibag: Vec::new(),
        igen: Vec::new(),
        shdr: Vec::new(),
    };
    for (id, b) in walk_list(pdta) {
        match id {
            b"phdr" => {
                for r in b.chunks_exact(38) {
                    s.phdr.push(PresetHdr {
                        name: name20(r, 0),
                        preset: rd16(r, 20),
                        bank: rd16(r, 22),
                        bag: rd16(r, 24),
                    });
                }
            }
            b"pbag" => {
                for r in b.chunks_exact(4) {
                    s.pbag.push(Bag {
                        generator_index: rd16(r, 0),
                        _mod: rd16(r, 2),
                    });
                }
            }
            b"pgen" => {
                for r in b.chunks_exact(4) {
                    s.pgen.push(Gen {
                        oper: rd16(r, 0),
                        amount: rd16(r, 2),
                    });
                }
            }
            b"inst" => {
                for r in b.chunks_exact(22) {
                    s.inst.push(InstHdr {
                        name: name20(r, 0),
                        bag: rd16(r, 20),
                    });
                }
            }
            b"ibag" => {
                for r in b.chunks_exact(4) {
                    s.ibag.push(Bag {
                        generator_index: rd16(r, 0),
                        _mod: rd16(r, 2),
                    });
                }
            }
            b"igen" => {
                for r in b.chunks_exact(4) {
                    s.igen.push(Gen {
                        oper: rd16(r, 0),
                        amount: rd16(r, 2),
                    });
                }
            }
            b"shdr" => {
                for r in b.chunks_exact(46) {
                    s.shdr.push(Shdr {
                        name: name20(r, 0),
                        start: rd32(r, 20),
                        end: rd32(r, 24),
                        start_loop: rd32(r, 28),
                        end_loop: rd32(r, 32),
                        rate: rd32(r, 36),
                        pitch: r[40],
                        correction: r[41] as i8,
                        link: rd16(r, 42),
                        ty: rd16(r, 44),
                    });
                }
            }
            _ => {}
        }
    }
    if s.phdr.len() < 2 || s.inst.len() < 2 || s.shdr.len() < 2 {
        return Err("SF2 sin registros suficientes (¿faltan los terminales?)".into());
    }
    Ok(s)
}

// -- escritura ---------------------------------------------------------------

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

// ---------------------------------------------------------------------------

/// Summary of the data retained by a SoundFont bake.
pub struct BakeReport {
    /// Number of retained presets.
    pub presets: usize,
    /// Number of retained instruments.
    pub instruments: usize,
    /// Number of retained samples.
    pub samples: usize,
    /// Size of the baked SF2 in bytes.
    pub bytes: usize,
    /// Requested `(bank, preset)` pairs that were absent from the source.
    pub missing: Vec<(u16, u16)>,
}

/// Lists `(bank, preset, name)` records without loading the synthesizer.
///
/// Only preset metadata is inspected, allowing valid records to be discovered
/// even when unrelated sample data is damaged.
///
/// # Errors
///
/// Returns an error when the RIFF/SF2 metadata structure is invalid.
pub fn list_presets(src: &[u8]) -> Result<Vec<(u16, u16, String)>, String> {
    // NO usa parse(): ése copia el chunk `smpl` entero a un Vec<i16>, y para
    // leer una tabla de nombres eso es medio giga de muestras en un SF2 grande
    // (en una colección real hay uno de 988 MB). Sólo hace falta pdta.
    let pdta = find_pdta(src).ok_or("SF2 sin chunk pdta")?;
    presets_from_pdta(pdta)
}

/// El cuerpo del `LIST pdta` dentro de un SF2 en memoria.
fn find_pdta(src: &[u8]) -> Option<&[u8]> {
    if src.len() < 12 || &src[0..4] != b"RIFF" || &src[8..12] != b"sfbk" {
        return None;
    }
    let mut at = 12;
    while at + 8 <= src.len() {
        let sz = rd32(src, at + 4) as usize;
        let body_at = at + 8;
        if body_at + sz > src.len() {
            return None;
        }
        if &src[at..at + 4] == b"LIST" && sz >= 4 && &src[body_at..body_at + 4] == b"pdta" {
            return Some(&src[body_at..body_at + sz]);
        }
        at = body_at + sz + (sz & 1);
    }
    None
}

fn presets_from_pdta(pdta: &[u8]) -> Result<Vec<(u16, u16, String)>, String> {
    for (id, b) in walk_list(pdta) {
        if id != b"phdr" {
            continue;
        }
        let mut v: Vec<(u16, u16, String)> = b
            .chunks_exact(38)
            .map(|r| (rd16(r, 22), rd16(r, 20), name20(r, 0)))
            .collect();
        v.pop(); // el terminal "EOP" no es un preset
        return Ok(v);
    }
    Err("pdta sin phdr".into())
}

/// Lists presets from a file while seeking past sample data.
///
/// # Errors
///
/// Returns an error for I/O failures or malformed RIFF/SF2 metadata.
pub fn list_presets_file(path: &str) -> Result<Vec<(u16, u16, String)>, String> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let end = f.metadata().map_err(|e| e.to_string())?.len();
    let mut hdr = [0u8; 12];
    f.read_exact(&mut hdr).map_err(|e| e.to_string())?;
    if &hdr[0..4] != b"RIFF" || &hdr[8..12] != b"sfbk" {
        return Err("no es un SF2 (falta RIFF/sfbk)".into());
    }
    let mut at: u64 = 12;
    while at + 8 <= end {
        f.seek(SeekFrom::Start(at)).map_err(|e| e.to_string())?;
        let mut ch = [0u8; 12];
        let want = if at + 12 <= end { 12 } else { 8 };
        f.read_exact(&mut ch[..want]).map_err(|e| e.to_string())?;
        let sz = u32::from_le_bytes([ch[4], ch[5], ch[6], ch[7]]) as u64;
        if &ch[0..4] == b"LIST" && want == 12 && &ch[8..12] == b"pdta" {
            if at + 8 + sz > end {
                return Err("pdta truncado".into());
            }
            let mut body = vec![0u8; sz as usize];
            f.seek(SeekFrom::Start(at + 8)).map_err(|e| e.to_string())?;
            f.read_exact(&mut body).map_err(|e| e.to_string())?;
            return presets_from_pdta(&body);
        }
        at += 8 + sz + (sz & 1);
    }
    Err("SF2 sin chunk pdta".into())
}

/// Bakes the requested presets into a self-contained SF2 named `name`.
///
/// Preset, instrument, generator, and sample indexes are rebuilt so retained
/// references remain valid.
///
/// # Errors
///
/// Returns an error when the source is malformed or none of the requested
/// presets can be retained.
pub fn bake(src: &[u8], keep: &[(u16, u16)], name: &str) -> Result<(Vec<u8>, BakeReport), String> {
    // SF3 transparente: el horneador lee el smpl como i16 crudo, así que un
    // origen con samples Vorbis se descomprime acá y el recorte que viaja al
    // pack es SIEMPRE SF2 plano (el runtime no paga decode al abrir).
    let src = crate::sf3::ensure_sf2(src);
    let s = parse(&src)?;

    // -- 1. seleccionar presets -------------------------------------------
    // El ÚLTIMO registro de phdr es el terminal "EOP": marca el fin de los bags
    // del anteúltimo y no es un preset.
    let mut want: Vec<usize> = Vec::new();
    let mut missing: Vec<(u16, u16)> = Vec::new();
    for &(bank, preset) in keep {
        match s.phdr[..s.phdr.len() - 1]
            .iter()
            .position(|p| p.bank == bank && p.preset == preset)
        {
            Some(i) => {
                if !want.contains(&i) {
                    want.push(i)
                }
            }
            None => missing.push((bank, preset)),
        }
    }
    if want.is_empty() {
        return Err("ninguno de los presets pedidos está en el SF2".into());
    }
    want.sort_unstable();

    // -- 2. de presets a instrumentos, y de instrumentos a muestras --------
    // Una zona SIN el generador terminal (instrument / sampleID) es una zona
    // GLOBAL: lleva defaults para las demás zonas del mismo registro. Se copia
    // tal cual, pero no aporta referencia que seguir.
    let mut inst_used: Vec<usize> = Vec::new();
    for &pi in &want {
        let b0 = s.phdr[pi].bag as usize;
        let b1 = s.phdr[pi + 1].bag as usize;
        for bi in b0..b1.min(s.pbag.len()) {
            let g0 = s.pbag[bi].generator_index as usize;
            let g1 = if bi + 1 < s.pbag.len() {
                s.pbag[bi + 1].generator_index as usize
            } else {
                s.pgen.len()
            };
            for g in &s.pgen[g0.min(s.pgen.len())..g1.min(s.pgen.len())] {
                if g.oper == GEN_INSTRUMENT {
                    let idx = g.amount as usize;
                    if idx + 1 < s.inst.len() && !inst_used.contains(&idx) {
                        inst_used.push(idx);
                    }
                }
            }
        }
    }
    inst_used.sort_unstable();

    let mut smp_used: Vec<usize> = Vec::new();
    for &ii in &inst_used {
        let b0 = s.inst[ii].bag as usize;
        let b1 = s.inst[ii + 1].bag as usize;
        for bi in b0..b1.min(s.ibag.len()) {
            let g0 = s.ibag[bi].generator_index as usize;
            let g1 = if bi + 1 < s.ibag.len() {
                s.ibag[bi + 1].generator_index as usize
            } else {
                s.igen.len()
            };
            for g in &s.igen[g0.min(s.igen.len())..g1.min(s.igen.len())] {
                if g.oper == GEN_SAMPLE_ID {
                    let idx = g.amount as usize;
                    if idx + 1 < s.shdr.len() && !smp_used.contains(&idx) {
                        smp_used.push(idx);
                        // Un sample estéreo apunta a su pareja por `link`; sin
                        // ella el otro canal queda mudo.
                        let l = s.shdr[idx].link as usize;
                        if s.shdr[idx].ty != 1 && l + 1 < s.shdr.len() && !smp_used.contains(&l) {
                            smp_used.push(l);
                        }
                    }
                }
            }
        }
    }
    smp_used.sort_unstable();

    // -- 3. emitir muestras y su tabla ------------------------------------
    let mut smpl: Vec<i16> = Vec::new();
    let mut shdr_out: Vec<u8> = Vec::new();
    let mut smp_map = std::collections::HashMap::new();
    for (new_i, &si) in smp_used.iter().enumerate() {
        smp_map.insert(si, new_i as u16);
    }
    for &si in &smp_used {
        let h = &s.shdr[si];
        let (a, b) = (h.start as usize, h.end as usize);
        if b > s.smpl.len() || a > b {
            return Err(format!("sample '{}' con offsets fuera del smpl", h.name));
        }
        let new_start = smpl.len() as u32;
        smpl.extend_from_slice(&s.smpl[a..b]);
        let new_end = smpl.len() as u32;
        smpl.extend(std::iter::repeat_n(0i16, SAMPLE_PAD));

        put_name(&mut shdr_out, &h.name);
        shdr_out.extend_from_slice(&new_start.to_le_bytes());
        shdr_out.extend_from_slice(&new_end.to_le_bytes());
        // Los loops son ABSOLUTOS en el smpl, así que se re-basan igual.
        shdr_out
            .extend_from_slice(&(new_start + h.start_loop.saturating_sub(h.start)).to_le_bytes());
        shdr_out.extend_from_slice(&(new_start + h.end_loop.saturating_sub(h.start)).to_le_bytes());
        shdr_out.extend_from_slice(&h.rate.to_le_bytes());
        shdr_out.push(h.pitch);
        shdr_out.push(h.correction as u8);
        shdr_out.extend_from_slice(
            &smp_map
                .get(&(h.link as usize))
                .copied()
                .unwrap_or(0)
                .to_le_bytes(),
        );
        shdr_out.extend_from_slice(&h.ty.to_le_bytes());
    }
    put_name(&mut shdr_out, "EOS");
    shdr_out.extend_from_slice(&[0u8; 26]);

    // -- 4. instrumentos, con sus zonas y sampleID remapeado ---------------
    let mut inst_map = std::collections::HashMap::new();
    for (new_i, &ii) in inst_used.iter().enumerate() {
        inst_map.insert(ii, new_i as u16);
    }
    let mut inst_out: Vec<u8> = Vec::new();
    let mut ibag_out: Vec<u8> = Vec::new();
    let mut igen_out: Vec<u8> = Vec::new();
    let mut ibag_n: u16 = 0;
    let mut igen_n: u16 = 0;
    for &ii in &inst_used {
        put_name(&mut inst_out, &s.inst[ii].name);
        inst_out.extend_from_slice(&ibag_n.to_le_bytes());
        let b0 = s.inst[ii].bag as usize;
        let b1 = s.inst[ii + 1].bag as usize;
        for bi in b0..b1.min(s.ibag.len()) {
            ibag_out.extend_from_slice(&igen_n.to_le_bytes());
            ibag_out.extend_from_slice(&0u16.to_le_bytes()); // sin modulators
            ibag_n += 1;
            let g0 = s.ibag[bi].generator_index as usize;
            let g1 = if bi + 1 < s.ibag.len() {
                s.ibag[bi + 1].generator_index as usize
            } else {
                s.igen.len()
            };
            for g in &s.igen[g0.min(s.igen.len())..g1.min(s.igen.len())] {
                let amount = if g.oper == GEN_SAMPLE_ID {
                    match smp_map.get(&(g.amount as usize)) {
                        Some(&m) => m,
                        None => continue,
                    }
                } else {
                    g.amount
                };
                igen_out.extend_from_slice(&g.oper.to_le_bytes());
                igen_out.extend_from_slice(&amount.to_le_bytes());
                igen_n += 1;
            }
        }
    }
    put_name(&mut inst_out, "EOI");
    inst_out.extend_from_slice(&ibag_n.to_le_bytes());
    ibag_out.extend_from_slice(&igen_n.to_le_bytes());
    ibag_out.extend_from_slice(&0u16.to_le_bytes());
    igen_out.extend_from_slice(&[0u8; 4]);

    // -- 5. presets, con instrument remapeado ------------------------------
    let mut phdr_out: Vec<u8> = Vec::new();
    let mut pbag_out: Vec<u8> = Vec::new();
    let mut pgen_out: Vec<u8> = Vec::new();
    let mut pbag_n: u16 = 0;
    let mut pgen_n: u16 = 0;
    for &pi in &want {
        put_name(&mut phdr_out, &s.phdr[pi].name);
        phdr_out.extend_from_slice(&s.phdr[pi].preset.to_le_bytes());
        phdr_out.extend_from_slice(&s.phdr[pi].bank.to_le_bytes());
        phdr_out.extend_from_slice(&pbag_n.to_le_bytes());
        phdr_out.extend_from_slice(&[0u8; 12]); // library/genre/morphology
        let b0 = s.phdr[pi].bag as usize;
        let b1 = s.phdr[pi + 1].bag as usize;
        for bi in b0..b1.min(s.pbag.len()) {
            pbag_out.extend_from_slice(&pgen_n.to_le_bytes());
            pbag_out.extend_from_slice(&0u16.to_le_bytes());
            pbag_n += 1;
            let g0 = s.pbag[bi].generator_index as usize;
            let g1 = if bi + 1 < s.pbag.len() {
                s.pbag[bi + 1].generator_index as usize
            } else {
                s.pgen.len()
            };
            for g in &s.pgen[g0.min(s.pgen.len())..g1.min(s.pgen.len())] {
                let amount = if g.oper == GEN_INSTRUMENT {
                    match inst_map.get(&(g.amount as usize)) {
                        Some(&m) => m,
                        None => continue,
                    }
                } else {
                    g.amount
                };
                pgen_out.extend_from_slice(&g.oper.to_le_bytes());
                pgen_out.extend_from_slice(&amount.to_le_bytes());
                pgen_n += 1;
            }
        }
    }
    put_name(&mut phdr_out, "EOP");
    phdr_out.extend_from_slice(&[0u8; 4]); // preset + bank
    phdr_out.extend_from_slice(&pbag_n.to_le_bytes());
    phdr_out.extend_from_slice(&[0u8; 12]);
    pbag_out.extend_from_slice(&pgen_n.to_le_bytes());
    pbag_out.extend_from_slice(&0u16.to_le_bytes());
    pgen_out.extend_from_slice(&[0u8; 4]);

    // -- 6. armar el archivo ----------------------------------------------
    let mut info = Vec::new();
    info.extend_from_slice(b"INFO");
    info.extend(chunk(b"ifil", &[2, 0, 1, 0]));
    info.extend(chunk(b"isng", b"EMU8000\0"));
    let mut nm = name.as_bytes().to_vec();
    nm.push(0);
    if nm.len() % 2 == 1 {
        nm.push(0);
    }
    info.extend(chunk(b"INAM", &nm));

    let mut smpl_bytes = Vec::with_capacity(smpl.len() * 2);
    for v in &smpl {
        smpl_bytes.extend_from_slice(&v.to_le_bytes());
    }
    let mut sdta = Vec::new();
    sdta.extend_from_slice(b"sdta");
    sdta.extend(chunk(b"smpl", &smpl_bytes));

    let mut pdta = Vec::new();
    pdta.extend_from_slice(b"pdta");
    pdta.extend(chunk(b"phdr", &phdr_out));
    pdta.extend(chunk(b"pbag", &pbag_out));
    pdta.extend(chunk(b"pmod", &[0u8; 10])); // sólo el terminal: ver la nota del módulo
    pdta.extend(chunk(b"pgen", &pgen_out));
    pdta.extend(chunk(b"inst", &inst_out));
    pdta.extend(chunk(b"ibag", &ibag_out));
    pdta.extend(chunk(b"imod", &[0u8; 10]));
    pdta.extend(chunk(b"igen", &igen_out));
    pdta.extend(chunk(b"shdr", &shdr_out));

    let mut body = Vec::new();
    body.extend_from_slice(b"sfbk");
    body.extend(chunk(b"LIST", &info));
    body.extend(chunk(b"LIST", &sdta));
    body.extend(chunk(b"LIST", &pdta));
    let out = chunk(b"RIFF", &body);

    let report = BakeReport {
        presets: want.len(),
        instruments: inst_used.len(),
        samples: smp_used.len(),
        bytes: out.len(),
        missing,
    };
    Ok((out, report))
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sf2::Sf2Synth;

    /// Un SF2 de origen con DOS presets, para poder verificar que el horneado
    /// se lleva uno y deja el otro. Con un solo preset el test no distinguiría
    /// «filtra bien» de «copia todo».
    fn fuente_dos_presets() -> Vec<u8> {
        const SR: u32 = 22050;
        const N: usize = 512;
        let mut smpl: Vec<u8> = Vec::new();
        // Dos samples distintos: seno y diente de sierra.
        for i in 0..N {
            let v = ((i as f32 / SR as f32) * 440.0 * std::f32::consts::TAU).sin() * 16000.0;
            smpl.extend_from_slice(&(v as i16).to_le_bytes());
        }
        smpl.extend(std::iter::repeat_n(0u8, SAMPLE_PAD * 2));
        let seg = smpl.len() / 2;
        for i in 0..N {
            let v = ((i as f32 / N as f32) * 2.0 - 1.0) * 12000.0;
            smpl.extend_from_slice(&(v as i16).to_le_bytes());
        }
        smpl.extend(std::iter::repeat_n(0u8, SAMPLE_PAD * 2));
        let _ = seg;

        fn nm(s: &str) -> [u8; 20] {
            let mut a = [0u8; 20];
            let b = s.as_bytes();
            let n = b.len().min(19);
            a[..n].copy_from_slice(&b[..n]);
            a
        }

        let mut phdr = Vec::new();
        for (n, pre, bag) in [("Uno", 0u16, 0u16), ("Dos", 7u16, 1u16), ("EOP", 0, 2)] {
            phdr.extend_from_slice(&nm(n));
            phdr.extend_from_slice(&pre.to_le_bytes());
            phdr.extend_from_slice(&0u16.to_le_bytes());
            phdr.extend_from_slice(&bag.to_le_bytes());
            phdr.extend_from_slice(&[0u8; 12]);
        }
        let mut pbag = Vec::new();
        for g in [0u16, 1, 2] {
            pbag.extend_from_slice(&g.to_le_bytes());
            pbag.extend_from_slice(&0u16.to_le_bytes());
        }
        let mut pgen = Vec::new();
        for inst in [0u16, 1] {
            pgen.extend_from_slice(&GEN_INSTRUMENT.to_le_bytes());
            pgen.extend_from_slice(&inst.to_le_bytes());
        }
        pgen.extend_from_slice(&[0u8; 4]);

        let mut inst = Vec::new();
        for (n, bag) in [("InstUno", 0u16), ("InstDos", 1u16), ("EOI", 2u16)] {
            inst.extend_from_slice(&nm(n));
            inst.extend_from_slice(&bag.to_le_bytes());
        }
        let mut ibag = Vec::new();
        for g in [0u16, 2, 4] {
            ibag.extend_from_slice(&g.to_le_bytes());
            ibag.extend_from_slice(&0u16.to_le_bytes());
        }
        let mut igen = Vec::new();
        for smp in [0u16, 1] {
            igen.extend_from_slice(&43u16.to_le_bytes()); // keyRange 0..127
            igen.extend_from_slice(&0x7F00u16.to_le_bytes());
            igen.extend_from_slice(&GEN_SAMPLE_ID.to_le_bytes());
            igen.extend_from_slice(&smp.to_le_bytes());
        }
        igen.extend_from_slice(&[0u8; 4]);

        let mut shdr = Vec::new();
        let a_end = N as u32;
        let b_start = (N + SAMPLE_PAD) as u32;
        let b_end = b_start + N as u32;
        for (n, s0, e0, ty) in [
            ("Seno", 0u32, a_end, 1u16),
            ("Sierra", b_start, b_end, 1u16),
            ("EOS", 0, 0, 0),
        ] {
            shdr.extend_from_slice(&nm(n));
            shdr.extend_from_slice(&s0.to_le_bytes());
            shdr.extend_from_slice(&e0.to_le_bytes());
            shdr.extend_from_slice(&s0.to_le_bytes());
            shdr.extend_from_slice(&e0.to_le_bytes());
            shdr.extend_from_slice(&SR.to_le_bytes());
            shdr.push(60);
            shdr.push(0);
            shdr.extend_from_slice(&0u16.to_le_bytes());
            shdr.extend_from_slice(&ty.to_le_bytes());
        }

        let mut info = Vec::new();
        info.extend_from_slice(b"INFO");
        info.extend(chunk(b"ifil", &[2, 0, 1, 0]));
        info.extend(chunk(b"isng", b"EMU8000\0"));
        info.extend(chunk(b"INAM", b"Fuente\0"));
        let mut sdta = Vec::new();
        sdta.extend_from_slice(b"sdta");
        sdta.extend(chunk(b"smpl", &smpl));
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
        chunk(b"RIFF", &body)
    }

    #[test]
    fn hornea_solo_el_preset_pedido() {
        let src = fuente_dos_presets();
        let (out, rep) = bake(&src, &[(0, 7)], "Horneado").expect("horneado");
        assert_eq!(rep.presets, 1);
        assert_eq!(rep.instruments, 1);
        assert_eq!(rep.samples, 1, "sólo la muestra del preset pedido");
        assert!(rep.missing.is_empty());
        assert!(out.len() < src.len(), "el horneado tiene que ser MÁS CHICO");

        // EL ORÁCULO: que el resultado CARGUE y SUENE. Un SF2 con los índices
        // mal remapeados carga igual y toca la muestra equivocada — o silencio.
        let s = Sf2Synth::new(&out, 48000).expect("el horneado tiene que cargar");
        let p = s.presets();
        assert_eq!(p.len(), 1);
        assert_eq!(
            (p[0].0, p[0].1),
            (0, 7),
            "conserva banco y preset ORIGINALES"
        );
    }

    #[test]
    fn el_horneado_suena() {
        let src = fuente_dos_presets();
        let (out, _) = bake(&src, &[(0, 7)], "Horneado").unwrap();
        let mut s = Sf2Synth::new(&out, 48000).unwrap();
        s.note_on(0, 60, 100);
        let mut buf = vec![0.0f32; 4096];
        s.render_interleaved(&mut buf);
        let peak = buf.iter().fold(0.0f32, |a, &b| a.max(b.abs()));
        assert!(
            peak > 1e-3,
            "el preset horneado tiene que sonar, pico {peak}"
        );
    }

    #[test]
    fn los_dos_presets_entran_si_se_piden_los_dos() {
        let src = fuente_dos_presets();
        let (out, rep) = bake(&src, &[(0, 0), (0, 7)], "Ambos").unwrap();
        assert_eq!(rep.presets, 2);
        assert_eq!(rep.samples, 2);
        let s = Sf2Synth::new(&out, 48000).unwrap();
        assert_eq!(s.presets().len(), 2);
    }

    /// Un preset que no está NO aborta el horneado: se reporta. Un mapeo puede
    /// quedar apuntando a algo que el artista borró, y tirar el pack entero por
    /// eso sería desproporcionado.
    #[test]
    fn un_preset_ausente_se_reporta_y_no_aborta() {
        let src = fuente_dos_presets();
        let (_, rep) = bake(&src, &[(0, 7), (9, 99)], "Con hueco").unwrap();
        assert_eq!(rep.presets, 1);
        assert_eq!(rep.missing, vec![(9, 99)]);
    }

    #[test]
    fn si_no_hay_ninguno_si_falla() {
        let src = fuente_dos_presets();
        assert!(bake(&src, &[(9, 99)], "Nada").is_err());
    }

    /// list_presets_file salta por las cabeceras RIFF con seeks en vez de leer
    /// el archivo entero — la biblioteca tiene 182 archivos y dos de ~1 GB. La
    /// aritmética de saltos falla en silencio (devuelve «sin pdta», que se ve
    /// igual que un archivo malo), así que se compara contra la versión en
    /// memoria: las dos tienen que dar EXACTAMENTE lo mismo.
    #[test]
    fn listar_presets_por_seek_da_lo_mismo_que_en_memoria() {
        let src = fuente_dos_presets();
        let en_memoria = list_presets(&src).expect("listar en memoria");
        assert_eq!(en_memoria.len(), 2, "la fuente tiene dos presets");

        let path = std::env::temp_dir().join("ayther_list_presets_seek.sf2");
        std::fs::write(&path, &src).expect("escribir el SF2 de prueba");
        let del_disco = list_presets_file(path.to_str().unwrap()).expect("listar por seek");
        let _ = std::fs::remove_file(&path);

        assert_eq!(del_disco, en_memoria);
    }

    /// El terminal "EOP" de phdr NO es un preset. Si se colara, la biblioteca
    /// mostraría una entrada fantasma al final de cada SoundFont.
    #[test]
    fn el_terminal_eop_no_se_lista() {
        let src = fuente_dos_presets();
        let l = list_presets(&src).unwrap();
        assert!(
            !l.iter().any(|(_, _, n)| n == "EOP"),
            "el registro terminal se coló en la lista: {l:?}"
        );
    }
}

#[cfg(test)]
mod tests_con_sf2_real {
    use super::*;
    use crate::sf2::Sf2Synth;

    /// Hornea contra un SF2 REAL y verifica que el resultado carga y suena.
    ///
    /// Los tests de arriba usan un SF2 sintético, que sólo ejercita el camino
    /// que yo mismo escribí. Las rarezas del formato —zonas globales, muestras
    /// estéreo enlazadas, generadores que no interpreto— sólo aparecen en
    /// archivos reales, y son exactamente donde un remapeo de índices se rompe
    /// SIN dar error: el archivo carga y toca la muestra equivocada.
    ///
    /// Se activa con AYTHER_SF2_TEST=<ruta>; sin eso se saltea, porque un test
    /// no puede depender de la colección de nadie.
    #[test]
    fn hornea_un_sf2_real() {
        let Ok(path) = std::env::var("AYTHER_SF2_TEST") else {
            eprintln!("[skip] sin AYTHER_SF2_TEST=<ruta a un .sf2>");
            return;
        };
        let src = std::fs::read(&path).expect("leer el SF2");
        // Se listan con el parser PROPIO, no con el sintetizador: RustySynth
        // valida el archivo entero y rechaza colecciones reales por un
        // instrumento roto en un rincón. El horneador no necesita esa
        // validación y puede rescatar los presets sanos.
        let presets = list_presets(&src).expect("listar presets del origen");
        assert!(!presets.is_empty(), "el SF2 de prueba no tiene presets");
        eprintln!(
            "[..] el origen carga en el sintetizador: {}",
            Sf2Synth::new(&src, 48000).is_ok()
        );
        eprintln!(
            "[..] origen: {} presets, {:.1} MB",
            presets.len(),
            src.len() as f64 / 1048576.0
        );

        // Los tres primeros, que es el orden de magnitud de un mapeo real.
        let keep: Vec<(u16, u16)> = presets.iter().take(3).map(|p| (p.0, p.1)).collect();
        let (out, rep) = bake(&src, &keep, "Horneado").expect("horneado");
        eprintln!(
            "[..] horneado: {} presets, {} instrumentos, {} muestras, {:.2} MB \
                   ({:.1}% del origen)",
            rep.presets,
            rep.instruments,
            rep.samples,
            rep.bytes as f64 / 1048576.0,
            100.0 * rep.bytes as f64 / src.len() as f64
        );

        assert!(
            rep.missing.is_empty(),
            "los presets pedidos salieron del propio origen"
        );
        assert!(out.len() < src.len(), "el horneado tiene que ser más chico");

        let mut s = Sf2Synth::new(&out, 48000).expect("el horneado tiene que cargar");
        assert_eq!(
            s.presets().len(),
            keep.len(),
            "conserva los presets pedidos"
        );

        // Que SUENE, que es lo que un remapeo roto no garantiza. Se prueban
        // varias notas: un preset puede tener zonas por rango de teclas y una
        // sola nota podría caer en la única que quedó bien por casualidad.
        let mut sono = 0;
        for key in [48, 60, 72] {
            s.all_notes_off();
            let mut warm = vec![0.0f32; 2048];
            s.render_interleaved(&mut warm);
            s.note_on(0, key, 100);
            let mut buf = vec![0.0f32; 8192];
            s.render_interleaved(&mut buf);
            let peak = buf.iter().fold(0.0f32, |a, &b| a.max(b.abs()));
            eprintln!("[..] nota {key}: pico {peak:.4}");
            if peak > 1e-3 {
                sono += 1;
            }
        }
        assert!(sono > 0, "ninguna de las tres notas sonó en el horneado");
    }
}
