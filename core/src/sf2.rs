//! SoundFont-based resynthesis for detected game instruments.
//!
//! [`Sf2Synth`] keeps timing under game control while replacing FM or PSG
//! timbres with SoundFont presets. Parsed fonts are shared through a cache.

// Re-síntesis con SoundFont.
//
// QUÉ RESUELVE. La sustitución por Secuencia que ya existe reemplaza un pasaje
// entero por una grabación: hay que rearreglar y regrabar afuera, y sólo calza
// si el timing coincide. Un SoundFont es otro eje, complementario — el juego
// SIGUE tocando (su tempo, sus jingles, sus cortes cuando te matan) y se cambia
// sólo el TIMBRE de una voz. No puede desincronizar, porque la fuente de tiempo
// es el juego.
//
// POR QUÉ ACÁ Y NO EN EL ENGINE. `ayther_engine` es una lib ESTÁTICA, así que
// un sintetizador LGPL (FluidSynth) obligaría a distribución dinámica o a
// entregar objetos relinkeables — la misma frontera que en hizo elegir
// libvpx sobre FFmpeg. Del lado Rust la frontera FFI ya existe y la pregunta de
// linkeo desaparece: RustySynth es MIT y no arrastra dependencias fuera de la
// std.
//
// ALCANCE. El 49,5% del bus del Mega Drive son escrituras al DAC — batería y
// voces digitalizadas, que son PCM y quedan FUERA de lo que un SoundFont puede
// cubrir; eso sigue por la sustitución de samples. Esto cubre la otra mitad:
// las voces FM y PSG que sí son notas.

use std::collections::HashMap;
use std::sync::{Arc, Mutex, OnceLock};

use rustysynth::{SoundFont, Synthesizer, SynthesizerSettings};

/// SoundFonts parseados, compartidos entre instancias (ver `new_shared`).
fn font_cache() -> &'static Mutex<HashMap<u64, Arc<SoundFont>>> {
    static C: OnceLock<Mutex<HashMap<u64, Arc<SoundFont>>>> = OnceLock::new();
    C.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Deterministic MIDI synthesizer backed by a SoundFont.
///
/// Rendering advances only by the requested number of samples and never uses a
/// background thread or wall clock.
pub struct Sf2Synth {
    synth: Synthesizer,
    /// Buffers de trabajo, reusados entre llamadas: `render` pide slices
    /// separados por canal y reservar por llamada sería basura en el camino
    /// caliente del audio.
    left: Vec<f32>,
    right: Vec<f32>,
}

impl Sf2Synth {
    /// Parses a SoundFont from memory for the requested output sample rate.
    ///
    /// # Errors
    ///
    /// Returns an error when the normalized SF2 data cannot be parsed or the
    /// synthesizer cannot be initialized.
    pub fn new(sf2: &[u8], sample_rate: i32) -> Result<Self, String> {
        // SF3 transparente: si los bytes traen samples Vorbis se convierten
        // acá, en la frontera — RustySynth sólo ve SF2 plano.
        let sf2 = crate::sf3::ensure_sf2(sf2);
        let mut cur = std::io::Cursor::new(&sf2[..]);
        let font = SoundFont::new(&mut cur).map_err(|e| format!("SoundFont: {e}"))?;
        let mut settings = SynthesizerSettings::new(sample_rate);

        // REVERB Y CHORUS APAGADOS, y no es por ahorrar CPU.
        //
        // Lo encontró un test: tras `all_notes_off` la cola seguía sonando a
        // 0,0067 de pico. Es la reverb, que RustySynth trae encendida. Y un
        // corte que no corta es un defecto REAL acá: el juego corta de golpe
        // —te matan, cambia la pantalla, arranca un jingle— y una cola
        // artificial sonaría por encima de lo que sigue.
        //
        // Además el timbre entra en la mezcla que el juego ya hace: agregarle
        // un espacio que nadie autoró cambia el carácter de la voz. Si alguna
        // vez se quiere reverb, va como decisión de autoría sobre el bus, no
        // como default del sintetizador.
        settings.enable_reverb_and_chorus = false;

        let synth = Synthesizer::new(&Arc::new(font), &settings)
            .map_err(|e| format!("Synthesizer: {e}"))?;
        Ok(Self {
            synth,
            left: Vec::new(),
            right: Vec::new(),
        })
    }

    /// Creates a synthesizer while sharing the parsed SoundFont identified by
    /// `key` with other instances.
    ///
    /// # Errors
    ///
    /// Returns an error when the font cache is poisoned or the data is invalid.
    pub fn new_shared(key: u64, sf2: &[u8], sample_rate: i32) -> Result<Self, String> {
        let font = {
            let mut cache = font_cache().lock().map_err(|_| "cache envenenada")?;
            match cache.get(&key) {
                Some(f) => Arc::clone(f),
                None => {
                    // SF3 transparente, SÓLO en el miss: el convertido queda
                    // parseado en el cache y las N instancias no lo re-pagan.
                    let sf2 = crate::sf3::ensure_sf2(sf2);
                    let mut cur = std::io::Cursor::new(&sf2[..]);
                    let f =
                        Arc::new(SoundFont::new(&mut cur).map_err(|e| format!("SoundFont: {e}"))?);
                    cache.insert(key, Arc::clone(&f));
                    f
                }
            }
        };
        let mut settings = SynthesizerSettings::new(sample_rate);
        settings.enable_reverb_and_chorus = false; // mismo motivo que en `new`
        let synth = Synthesizer::new(&font, &settings).map_err(|e| format!("Synthesizer: {e}"))?;
        Ok(Self {
            synth,
            left: Vec::new(),
            right: Vec::new(),
        })
    }

    /// Removes cached SoundFonts that are no longer used by any synthesizer.
    pub fn trim_font_cache() {
        if let Ok(mut c) = font_cache().lock() {
            c.retain(|_, f| Arc::strong_count(f) > 1);
        }
    }

    /// Returns available presets as `(bank, preset, name)` tuples.
    pub fn presets(&self) -> Vec<(i32, i32, String)> {
        self.synth
            .get_sound_font()
            .get_presets()
            .iter()
            .map(|p| {
                (
                    p.get_bank_number(),
                    p.get_patch_number(),
                    p.get_name().to_string(),
                )
            })
            .collect()
    }

    /// Starts a MIDI note on `channel`.
    pub fn note_on(&mut self, channel: i32, key: i32, velocity: i32) {
        self.synth.note_on(channel, key, velocity);
    }

    /// Releases a MIDI note on `channel`.
    pub fn note_off(&mut self, channel: i32, key: i32) {
        self.synth.note_off(channel, key);
    }

    /// Selects a preset for a channel using MIDI Program Change.
    pub fn program_change(&mut self, channel: i32, preset: i32) {
        self.synth.process_midi_message(channel, 0xC0, preset, 0);
    }

    /// Sends a MIDI Control Change message.
    pub fn control_change(&mut self, channel: i32, cc: i32, value: i32) {
        self.synth.process_midi_message(channel, 0xB0, cc, value);
    }

    /// Stops every active voice immediately.
    pub fn all_notes_off(&mut self) {
        self.synth.note_off_all(true);
    }

    /// Renders interleaved stereo samples into `out`.
    ///
    /// An odd trailing element is left untouched because complete stereo frames
    /// require two elements.
    pub fn render_interleaved(&mut self, out: &mut [f32]) {
        let frames = out.len() / 2;
        if frames == 0 {
            return;
        }
        if self.left.len() < frames {
            self.left.resize(frames, 0.0);
            self.right.resize(frames, 0.0);
        }
        self.synth
            .render(&mut self.left[..frames], &mut self.right[..frames]);
        for i in 0..frames {
            out[i * 2] = self.left[i];
            out[i * 2 + 1] = self.right[i];
        }
    }

    /// Sets the synthesizer's master volume.
    pub fn set_master_volume(&mut self, v: f32) {
        self.synth.set_master_volume(v);
    }
}

// ---------------------------------------------------------------------------

/// Construye un SF2 mínimo VÁLIDO en memoria: un sample de onda senoidal, un
/// instrumento que lo cubre entero y un preset que apunta al instrumento.
///
/// Se genera en vez de traer un archivo porque un test no debería depender
/// de la red ni de un asset externo con su propia licencia. Y sirve de
/// documentación: esto es lo MÍNIMO que el formato exige, con sus registros
/// terminales, que es justo lo que se olvida al escribir un SF2 a mano.
///
/// `pub(crate)` bajo cfg(test): los tests de sf3/sfz parten de este mismo
/// archivo (le suben la versión, le marcan flags) en vez de duplicar 100
/// líneas de builder RIFF.
#[cfg(test)]
pub(crate) fn minimal_sf2() -> Vec<u8> {
    const SR: u32 = 44100;
    const N: usize = 1024; // un ciclo largo; el contenido no importa, la estructura sí

    // -- sample: seno de 440 Hz, i16 --------------------------------------
    let mut smpl: Vec<u8> = Vec::new();
    for i in 0..N {
        let t = i as f32 / SR as f32;
        let v = (t * 440.0 * std::f32::consts::TAU).sin() * 16000.0;
        smpl.extend_from_slice(&(v as i16).to_le_bytes());
    }
    // El spec exige 46 muestras de silencio DESPUÉS de cada sample.
    smpl.extend(std::iter::repeat_n(0u8, 46 * 2));

    fn chunk(id: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut v = Vec::with_capacity(8 + body.len() + 1);
        v.extend_from_slice(id);
        v.extend_from_slice(&(body.len() as u32).to_le_bytes());
        v.extend_from_slice(body);
        if body.len() % 2 == 1 {
            v.push(0); // los chunks RIFF van a tamaño par
        }
        v
    }
    fn name20(s: &str) -> [u8; 20] {
        let mut a = [0u8; 20];
        let b = s.as_bytes();
        let n = b.len().min(19);
        a[..n].copy_from_slice(&b[..n]);
        a
    }

    // -- INFO --------------------------------------------------------------
    let mut info = Vec::new();
    info.extend_from_slice(b"INFO");
    info.extend(chunk(b"ifil", &[2, 0, 1, 0])); // versión 2.1
    info.extend(chunk(b"isng", b"EMU8000\0"));
    info.extend(chunk(b"INAM", b"Ayther Test\0"));

    // -- sdta --------------------------------------------------------------
    let mut sdta = Vec::new();
    sdta.extend_from_slice(b"sdta");
    sdta.extend(chunk(b"smpl", &smpl));

    // -- pdta --------------------------------------------------------------
    // Cada sub-chunk necesita su registro TERMINAL; sin eso el parser
    // rechaza el archivo (o peor, lee de más).
    let mut phdr = Vec::new();
    for (nm, bag) in [("Test", 0u16), ("EOP", 1u16)] {
        phdr.extend_from_slice(&name20(nm));
        phdr.extend_from_slice(&0u16.to_le_bytes()); // preset
        phdr.extend_from_slice(&0u16.to_le_bytes()); // bank
        phdr.extend_from_slice(&bag.to_le_bytes());
        phdr.extend_from_slice(&0u32.to_le_bytes()); // library
        phdr.extend_from_slice(&0u32.to_le_bytes()); // genre
        phdr.extend_from_slice(&0u32.to_le_bytes()); // morphology
    }

    let mut pbag = Vec::new();
    for generator_index in [0u16, 1u16] {
        pbag.extend_from_slice(&generator_index.to_le_bytes());
        pbag.extend_from_slice(&0u16.to_le_bytes());
    }
    let pmod = vec![0u8; 10]; // sólo el terminal
    let mut pgen = Vec::new();
    pgen.extend_from_slice(&41u16.to_le_bytes()); // gen 41 = instrument
    pgen.extend_from_slice(&0u16.to_le_bytes());
    pgen.extend_from_slice(&[0, 0, 0, 0]); // terminal

    let mut inst = Vec::new();
    for (nm, bag) in [("Test", 0u16), ("EOI", 1u16)] {
        inst.extend_from_slice(&name20(nm));
        inst.extend_from_slice(&bag.to_le_bytes());
    }
    let mut ibag = Vec::new();
    for generator_index in [0u16, 2u16] {
        ibag.extend_from_slice(&generator_index.to_le_bytes());
        ibag.extend_from_slice(&0u16.to_le_bytes());
    }
    let imod = vec![0u8; 10];
    let mut igen = Vec::new();
    igen.extend_from_slice(&43u16.to_le_bytes()); // gen 43 = keyRange
    igen.extend_from_slice(&0x7F00u16.to_le_bytes()); // 0..127
    igen.extend_from_slice(&53u16.to_le_bytes()); // gen 53 = sampleID (va ÚLTIMO)
    igen.extend_from_slice(&0u16.to_le_bytes());
    igen.extend_from_slice(&[0, 0, 0, 0]); // terminal

    let mut shdr = Vec::new();
    let end = N as u32;
    for (nm, s, e, ls, le, sr, pitch, ty) in [
        ("TestSample", 0u32, end, 0u32, end, SR, 60u8, 1u16),
        ("EOS", 0, 0, 0, 0, 0, 0, 0),
    ] {
        shdr.extend_from_slice(&name20(nm));
        shdr.extend_from_slice(&s.to_le_bytes());
        shdr.extend_from_slice(&e.to_le_bytes());
        shdr.extend_from_slice(&ls.to_le_bytes());
        shdr.extend_from_slice(&le.to_le_bytes());
        shdr.extend_from_slice(&sr.to_le_bytes());
        shdr.push(pitch);
        shdr.push(0); // pitchCorrection
        shdr.extend_from_slice(&0u16.to_le_bytes()); // sampleLink
        shdr.extend_from_slice(&ty.to_le_bytes()); // 1 = monoSample
    }

    let mut pdta = Vec::new();
    pdta.extend_from_slice(b"pdta");
    pdta.extend(chunk(b"phdr", &phdr));
    pdta.extend(chunk(b"pbag", &pbag));
    pdta.extend(chunk(b"pmod", &pmod));
    pdta.extend(chunk(b"pgen", &pgen));
    pdta.extend(chunk(b"inst", &inst));
    pdta.extend(chunk(b"ibag", &ibag));
    pdta.extend(chunk(b"imod", &imod));
    pdta.extend(chunk(b"igen", &igen));
    pdta.extend(chunk(b"shdr", &shdr));

    let mut body = Vec::new();
    body.extend_from_slice(b"sfbk");
    body.extend(chunk(b"LIST", &info));
    body.extend(chunk(b"LIST", &sdta));
    body.extend(chunk(b"LIST", &pdta));
    chunk(b"RIFF", &body)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn carga_un_soundfont_y_lista_sus_presets() {
        let sf2 = minimal_sf2();
        let s = Sf2Synth::new(&sf2, 48000).expect("el SF2 mínimo tiene que cargar");
        let p = s.presets();
        assert_eq!(p.len(), 1, "un preset");
        assert_eq!((p[0].0, p[0].1), (0, 0), "banco 0, preset 0");
    }

    /// EL oráculo: que SUENE. Cargar el font y no fallar no prueba nada — un
    /// SF2 mal armado puede cargar y renderizar silencio.
    #[test]
    fn una_nota_produce_senal() {
        let sf2 = minimal_sf2();
        let mut s = Sf2Synth::new(&sf2, 48000).unwrap();

        // Silencio antes de tocar: si esto ya trae señal, lo que mide el test
        // siguiente no es la nota.
        let mut quiet = vec![0.0f32; 4096];
        s.render_interleaved(&mut quiet);
        let peak_quiet = quiet.iter().fold(0.0f32, |a, &b| a.max(b.abs()));
        assert!(
            peak_quiet < 1e-6,
            "sin notas tiene que haber silencio, hubo {peak_quiet}"
        );

        s.note_on(0, 60, 100);
        let mut buf = vec![0.0f32; 4096];
        s.render_interleaved(&mut buf);
        let peak = buf.iter().fold(0.0f32, |a, &b| a.max(b.abs()));
        assert!(
            peak > 1e-3,
            "la nota tiene que producir señal, pico fue {peak}"
        );

        // Y que el corte inmediato corte: es lo que usa el motor cuando el juego
        // cambia de escena, y una nota colgada ahí se oiría sobre lo que sigue.
        s.all_notes_off();
        let mut tail = vec![0.0f32; 16384];
        s.render_interleaved(&mut tail);
        let peak_tail = tail[tail.len() - 2048..]
            .iter()
            .fold(0.0f32, |a, &b| a.max(b.abs()));
        assert!(
            peak_tail < 1e-3,
            "tras all_notes_off el final tiene que callar, fue {peak_tail}"
        );
    }

    /// Determinismo: la misma secuencia de notas da EXACTAMENTE el mismo PCM.
    /// Sin esto un render con SF2 no sería reproducible y no serviría para el
    /// replay — que es la razón por la que exigía este oráculo antes de
    /// integrar nada.
    #[test]
    fn el_render_es_determinista() {
        let sf2 = minimal_sf2();
        let run = || {
            let mut s = Sf2Synth::new(&sf2, 48000).unwrap();
            let mut out = vec![0.0f32; 8192];
            s.note_on(0, 60, 100);
            s.render_interleaved(&mut out[..4096]);
            s.note_off(0, 60);
            s.render_interleaved(&mut out[4096..]);
            out
        };
        assert_eq!(
            run(),
            run(),
            "dos corridas idénticas tienen que dar el mismo PCM"
        );
    }
}
