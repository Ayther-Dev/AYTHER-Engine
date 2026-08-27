//! Deterministic audio-event detection from sound-chip commands.
//!
//! The detector combines YM2612, SN76489, and RF5C164 activity into stable
//! instrument and event identities suitable for authored substitutions.

// ---------------------------------------------------------------------------
// audio_event.rs — detección de eventos de audio por COMANDOS al chip (C-A2).
//
// ## Por qué
//
// Hashear el PCM de salida (audio_hasher.rs) es frágil: dos SFX simultáneos suman
// sus muestras → hash nuevo que no coincide con ninguno; y el PCM NO es reproducible
// a través del replay (la fase del FM diverge tras unserialize). En cambio, las
// ESCRITURAS a los registros de los chips de sonido (YM2612 FM + SN76489 PSG) son
// comandos que dispara la CPU, y la CPU es byte-determinista en replay — el spike
// `tools/audio_chip_spike` lo prueba: la secuencia de comandos es replay-estable.
//
// ## Qué hace
//
// Ingiere, por frame, el log de escrituras crudas del fork (ids 0x109/0x10A,
// `AudioWrite { chip, addr, data }`) y mantiene un SHADOW de los registros de cada
// chip. Detecta el ciclo de vida de cada canal:
//
//   * FM:  el registro 0x28 (key on/off) marca inicio/fin por canal (FM 0-5).
//   * PSG: la atenuación (0xF = mudo) marca inicio/fin por canal (PSG 0-3).
//   * PCM: el registro ON/OFF del RF5C164 de Sega CD (PCM 0-7).
//
// ## Dos caminos de entrada, UN dueño de la identidad
//
// El PCM llega distinto: el core NO expone el bus de ese chip, sólo eventos ya
// tipificados por `poll_audio_events` (key-on/off, volumen, pitch). Así que hay
// dos transportes —escrituras crudas para FM/PSG, eventos para PCM— pero una
// sola definición de qué es «el mismo sonido», y vive acá.
//
// Esa fue una decisión, no una casualidad. El core también sabe tipificar y
// hashear voces de FM, y se podría haber delegado todo en él. No se hizo porque
// las firmas ya AUTORADAS por el usuario (200 en Golden Axe y Aladdin al
// 2026-08-12) son las de este módulo: cambiar de fuente de identidad las
// invalida en bloque, y encima el core sólo hashea FM —PSG y PCM llegan sin
// identidad de timbre—, así que ni siquiera se retiraría este código. El core
// da los HECHOS; la identidad se decide en un único lugar.
//
// Cada transición apagado→encendido ABRE un bloque de actividad; encendido→apagado
// lo CIERRA en un `AudioEvent` con su rango de frames y una FIRMA estable. La firma
// es el hash del snapshot de los registros del canal en el momento del key-on (el
// "patch" del instrumento + la frecuencia) — el mismo SFX produce la misma firma,
// así el Lab agrupa apariciones idénticas (CU-A2) sin depender del PCM ni del timing.
//
// ## Lo que NO hace (a propósito)
//
// * No usa el `cycle` de la escritura: el timing intra-frame diverge en algunos
//   estados de replay (ver audio_chip_spike). La identidad es por `(chip,addr,data)`.
// * No agrupa canales en un solo evento: eso es autoría en el Lab (§2.5 del plan).
//   Acá cada canal produce sus propios bloques; el usuario los combina.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tipos públicos
// ---------------------------------------------------------------------------

/// YM2612 FM chip identifier used by the detector and C ABI.
pub const CHIP_FM: u8 = 0; // YM2612
/// SN76489 PSG chip identifier used by the detector and C ABI.
pub const CHIP_PSG: u8 = 1; // SN76489
/// Sega CD RF5C164 PCM chip identifier.
///
/// PCM arrives as typed [`PcmEvent`] values rather than raw register writes.
pub const CHIP_PCM: u8 = 3;

/// Channel that is currently keyed on, including its live substitution keys.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ActiveChannel {
    /// Stable event signature captured at key-on.
    pub signature: u64,
    /// Timbre identity captured at key-on; zero means unknown.
    pub instrument: u64, // fm/psg/pcm_instrument al key-on (0 = desconocido)
    /// One of [`CHIP_FM`], [`CHIP_PSG`], or [`CHIP_PCM`].
    pub chip: u8, // CHIP_FM | CHIP_PSG | CHIP_PCM
    /// Chip-local channel number.
    pub channel: u8, // FM 0-5 | PSG 0-3 | PCM 0-7
    /// MIDI note captured at key-on, or [`NO_PITCH`].
    pub pitch: u8, // nota MIDI al key-on; NO_PITCH = sin altura (DAC/ruido/PCM)
}

/// Raw audio-chip bus write emitted by the emulator core.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct AudioWrite {
    /// Cycle timestamp within the emulated frame.
    pub cycle: u32,
    /// Chip register address.
    pub addr: u16,
    /// Byte written to the register.
    pub data: u8,
    /// Chip identifier.
    pub chip: u8,
}

/// RF5C164 sample-start event kind.
pub const PCM_KEY_ON: u8 = 1;
/// RF5C164 sample-stop event kind.
pub const PCM_KEY_OFF: u8 = 2;
/// RF5C164 playback-rate event kind.
pub const PCM_PITCH: u8 = 6;
/// RF5C164 envelope or volume event kind.
pub const PCM_VOLUME: u8 = 7;

/// Typed Sega CD RF5C164 event unpacked from the emulator-core ABI.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct PcmEvent {
    /// One of the `PCM_*` event-kind constants.
    pub kind: u8, // PCM_KEY_ON | PCM_KEY_OFF | PCM_PITCH | PCM_VOLUME
    /// RF5C164 channel in `0..=7`.
    pub channel: u8, // 0-7
    /// Envelope multiplier used as volume.
    pub env: u8, // multiplicador de envolvente (volumen)
    /// Stereo pan register value.
    pub pan: u8,
    /// Sample start address register.
    pub st: u8, // registro ST: dirección de arranque en WAVE RAM
    /// Reserved padding byte from the C representation.
    pub _pad: u8,
    /// Sample loop address.
    pub ls: u16, // dirección de loop
    /// 5.11 fixed-point playback address increment.
    pub fd: u16, // incremento de dirección 5.11 (velocidad, no nota)
}

/// Activity of one audio channel between key-on and key-off.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AudioEvent {
    /// Stable hash of the channel-register snapshot at key-on.
    pub signature: u64,
    /// Timbre identity excluding pitch, channel, pan, and volume.
    pub instrument: u64,
    /// One of [`CHIP_FM`], [`CHIP_PSG`], or [`CHIP_PCM`].
    pub chip: u8, // CHIP_FM | CHIP_PSG | CHIP_PCM
    /// Chip-local channel number.
    pub channel: u8, // FM 0-5 | PSG 0-3 | PCM 0-7
    /// Frame at which the channel keyed on.
    pub start_frame: u32, // frame del key-on
    /// Frame at which the channel keyed off, inclusive.
    pub end_frame: u32, // frame del key-off (== start_frame si dura un frame)
    /// MIDI note in 0–127 decoded from the frequency registers at key-on.
    ///
    /// FM uses `fnum` and block, while PSG uses its 10-bit tone period.
    /// [`NO_PITCH`] represents DAC, PSG noise, or a zero frequency. PCM derives a
    /// relative pitch from playback increment `fd`: note 60 is the recorded speed,
    /// which is sufficient to distinguish the same sample at different rates.
    pub pitch: u8,
    /// MIDI-style velocity at key-on in 1–127.
    ///
    /// FM derives it from the carrier operator's Total Level, PSG from attenuation,
    /// and PCM from its envelope multiplier. [`NO_VELOCITY`] means unavailable,
    /// as for DAC and residual events. Volume is deliberately excluded from
    /// instrument identity.
    pub velocity: u8,
}

/// Sentinel used by [`AudioEvent::pitch`] when no note is available.
pub const NO_PITCH: u8 = 255;

/// Sentinel used by [`AudioEvent::velocity`] when velocity is unavailable.
///
/// This applies to DAC audio, whose level lives in PCM samples, and to residual
/// events that began before capture so their key-on state was not observed.
pub const NO_VELOCITY: u8 = 0;

/// «Velocidad» del PSG: 16 pasos de atenuación de 2 dB, 15 = silencio.
fn psg_velocity(atten: u8) -> u8 {
    let a = atten & 0x0F;
    if a >= 15 { 1 } else { 127 - a * 8 }
}

/// «Velocidad» del PCM: el multiplicador de envolvente (0-255) llevado a la
/// escala MIDI. Es el único de los tres chips donde el nivel es un valor lineal
/// directo y no una atenuación, así que acá no hay que invertir nada.
fn pcm_velocity(env: u8) -> u8 {
    let v = (env as u32 * 127) / 255;
    if v < 1 { 1 } else { v as u8 }
}

/// `fd` a 1:1 — el sample se lee a la velocidad a la que fue grabado.
/// El registro es punto fijo 5.11, así que 1.0 == 2048.
const PCM_FD_UNITY: u16 = 0x0800;
/// La nota que se le asigna a esa velocidad. Do central en MIDI: un ancla
/// arbitraria pero fija, para que los desvíos se lean como intervalos.
const PCM_PITCH_UNITY: i32 = 60;

/// Nota del PCM, derivada de la VELOCIDAD DE REPRODUCCIÓN.
///
/// Este chip no tiene tono: reproduce un sample más rápido o más lento, y eso
/// es exactamente lo que hace un sampler cuando transporta una nota. Así que la
/// razón `fd / 0x800` se lee como intervalo — doblar la velocidad es una octava
/// arriba— y se ancla a la nota 60 cuando el sample suena a su velocidad
/// original.
///
/// NO ES UNA ALTURA ABSOLUTA y no puede serlo: depende de a qué tono se grabó
/// el sample, cosa que el emulador no sabe. Es una altura RELATIVA al material,
/// que es lo único que el hardware determina. Sirve para lo que tiene que
/// servir: que la regla de match por instrumento+nota
/// distinga el mismo sample disparado a distintas velocidades, y que el
/// piano-roll los muestre separados.
fn pcm_pitch(fd: u16) -> u8 {
    if fd == 0 {
        return NO_PITCH;
    } // canal parado: no hay nota que mostrar
    let semitonos = 12.0 * (fd as f64 / PCM_FD_UNITY as f64).log2();
    let n = PCM_PITCH_UNITY + semitonos.round() as i32;
    if !(0..=127).contains(&n) {
        NO_PITCH
    } else {
        n as u8
    }
}

// ---------------------------------------------------------------------------
// Hashing (FNV-1a 64, determinista y sin dependencias)
// ---------------------------------------------------------------------------

const FNV_SEED: u64 = 1469598103934665603;
const FNV_PRIME: u64 = 1099511628211;

/// Operadores PORTADORES por algoritmo del YM2612 (bitmask sobre op 0..3).
/// En los algoritmos 0-3 sólo el operador 4 sale al DAC; a partir del 4 salen
/// varios, hasta el 7 donde los cuatro son portadores.
const FM_CARRIERS: [u8; 8] = [0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF];

/// DAC: un frame se considera "con señal" si el rango (max-min) de sus muestras
/// 0x2A llega a esto; por debajo es silencio (muestras casi constantes, cualquier
/// valor de reposo). 8/256 ≈ 3% — tolerante a ruido de cuantización.
const DAC_SILENCE_RANGE: u8 = 8;
/// Frames consecutivos de silencio que CIERRAN un bloque DAC (separa SFX/golpes).
/// ~6 frames ≈ 100 ms: separa sonidos distintos sin partir pausas internas breves.
const DAC_GAP_FRAMES: u32 = 6;

#[inline]
fn mix(h: u64, b: u8) -> u64 {
    (h ^ b as u64).wrapping_mul(FNV_PRIME)
}

// ---------------------------------------------------------------------------
// AudioEventDetector
// ---------------------------------------------------------------------------

/// Stateful detector that converts chip writes into channel-scoped events.
pub struct AudioEventDetector {
    // -- YM2612 (FM) --------------------------------------------------------
    /// Shadow de los 512 registros del FM (banco 0 = 0x000-0x0FF, banco 1 = 0x100-0x1FF).
    fm_regs: [u8; 0x200],
    // (el latch del FM lo resuelve el core desde el fork 3fc6ee89 — ver apply_fm)
    fm_on: [bool; 6],
    fm_start: [u32; 6],
    fm_sig: [u64; 6],
    fm_inst: [u64; 6], // instrument capturado al key-on (ídem fm_sig)
    /// El canal ya tuvo actividad de key esta toma. Un key-OFF sin key-on previo
    /// = nota que venía sonando desde el ESTADO INICIAL de la toma (el key-on
    /// ocurrió antes de grabar): se sintetiza un evento RESIDUAL [0, off-1] para
    /// que ese sonido audible tenga representación (reporte 2026-07-21, FM 3 de
    /// Demo Amazona sonaba al inicio sin bloque).
    fm_seen: [bool; 6],
    /// EVIDENCIA de audio para el residual: máscara de canales que SUENAN al
    /// inicio de la toma (bits 0-5 FM · 6-9 PSG), medida por el engine con una
    /// sonda de PCM por canal aislado (set_initial_active). Sin el bit, el
    /// key-off/mute huérfano se ignora — los drivers mutean canales SILENCIOSOS
    /// al inicializar la música y generaban residuales espurios (PSG 1-4 con
    /// bloques sin nada que ver ni oír, reporte 2026-07-21; la evidencia por
    /// escrituras no alcanzó: GA escribe frecuencias PSG aunque el canal calle).
    initial_active: u16,

    // -- DAC (FM6 en modo DAC: reg 0x2B enable + 0x2A data) -----------------
    // El DAC es un stream PCM continuo, sin key-on/off que delimite sonidos. Se
    // SEGMENTA por SILENCIO: un frame es "con señal" si sus muestras 0x2A varían
    // (max-min ≥ DAC_SILENCE_RANGE); silencio = muestras casi constantes (cualquier
    // valor de reposo) o sin data. Un bloque abre al primer frame con señal y cierra
    // cuando el silencio dura ≥ DAC_GAP_FRAMES (termina en el último frame con señal).
    // Así los SFX/golpes discretos quedan como eventos separados; sólo el stream
    // genuinamente continuo (sin silencios) queda como un bloque. La firma hashea los
    // bytes PCM del frame de inicio (replay-estables → el mismo SFX da la misma firma).
    dac_enabled: bool,
    dac_on: bool,
    dac_start: u32,
    dac_sig: u64,
    dac_last_loud: u32, // último frame con señal (= end_frame del bloque)
    dac_silence: u32,   // frames consecutivos de silencio con el bloque abierto
    dac_wrote: bool,    // hubo escrituras a 0x2A este frame
    dac_min: u8,        // rango de muestras del frame (detección de silencio)
    dac_max: u8,
    dac_frame_hash: u64, // hash de los bytes DAC del frame (semilla de la firma)

    // -- SN76489 (PSG) ------------------------------------------------------
    psg_latch: u8,
    psg_freq: [u16; 4], // frecuencia/control por canal (10 bits tono; control de ruido en ch3)
    psg_on: [bool; 4],
    psg_start: [u32; 4],
    psg_sig: [u64; 4],
    psg_inst: [u64; 4],  // instrument capturado al key-on (ídem psg_sig)
    psg_seen: [bool; 4], // ídem fm_seen (residual del estado inicial)

    // -- RF5C164 (PCM de Sega CD, ) -------------------------------------
    // 8 canales, alimentados por eventos YA TIPIFICADOS (el core no expone el
    // bus de este chip). No hay evento RESIDUAL: `initial_active` es una máscara
    // de 16 bits ocupada por los diez canales de FM y PSG, y la sonda de PCM por
    // canal aislado que la mide tampoco cubre este chip. Un sample que ya venía
    // sonando al empezar la toma queda sin bloque — mismo trato que el DAC.
    pcm_on: [bool; 8],
    pcm_start: [u32; 8],
    pcm_sig: [u64; 8],
    pcm_inst: [u64; 8],
    pcm_vel: [u8; 8],
    pcm_pitch: [u8; 8],

    // -- Pitch (Mezclar): nota MIDI capturada AL key-on, por canal ----------
    fm_pitch: [u8; 6],
    psg_pitch: [u8; 4],
    /// «Velocidad» capturada al key-on, igual que el pitch. En FM
    /// sale del TL del portador; en PSG, de la atenuación.
    fm_vel: [u8; 6],
    psg_vel: [u8; 4],
    /// Región del reloj (false = NTSC): afecta la conversión fnum/tono → Hz.
    pal: bool,

    // -- Salida -------------------------------------------------------------
    events: Vec<AudioEvent>,
    frame: u32,
}

impl Default for AudioEventDetector {
    fn default() -> Self {
        Self::new()
    }
}

impl AudioEventDetector {
    /// Creates a detector with all channels inactive.
    pub fn new() -> Self {
        Self {
            fm_regs: [0; 0x200],
            fm_on: [false; 6],
            fm_start: [0; 6],
            fm_sig: [0; 6],
            fm_inst: [0; 6],
            fm_seen: [false; 6],
            initial_active: 0,
            dac_enabled: false,
            dac_on: false,
            dac_start: 0,
            dac_sig: 0,
            dac_last_loud: 0,
            dac_silence: 0,
            dac_wrote: false,
            dac_min: 255,
            dac_max: 0,
            dac_frame_hash: 0,
            psg_latch: 0,
            psg_freq: [0; 4],
            psg_on: [false; 4],
            psg_start: [0; 4],
            psg_sig: [0; 4],
            psg_inst: [0; 4],
            psg_seen: [false; 4],
            pcm_on: [false; 8],
            pcm_start: [0; 8],
            pcm_sig: [0; 8],
            pcm_inst: [0; 8],
            pcm_vel: [0; 8],
            pcm_pitch: [NO_PITCH; 8],
            fm_pitch: [NO_PITCH; 6],
            psg_pitch: [NO_PITCH; 4],
            fm_vel: [0; 6],
            psg_vel: [0; 4],
            pal: false,
            events: Vec::new(),
            frame: 0,
        }
    }

    /// Resets all detector state for a new capture or analysis pass.
    pub fn reset(&mut self) {
        *self = Self::new();
    }

    /// Processes one frame of chip writes and advances event detection.
    ///
    /// Call once per frame in increasing frame order. This is equivalent to
    /// [`Self::process_frame_ex`] with no PCM events and is convenient for systems
    /// that do not expose the Sega CD PCM chip.
    pub fn process_frame(&mut self, frame: u32, writes: &[AudioWrite]) {
        self.process_frame_ex(frame, writes, &[]);
    }

    /// Processes one frame from both supported audio-event paths.
    ///
    /// `writes` contains raw FM and PSG bus writes. `pcm` contains already typed
    /// Sega CD RF5C164 events because that chip's bus is not exposed. Call exactly
    /// once per frame so both paths share the same frame number; their relative
    /// order within the frame is irrelevant.
    pub fn process_frame_ex(&mut self, frame: u32, writes: &[AudioWrite], pcm: &[PcmEvent]) {
        self.frame = frame;
        self.dac_wrote = false;
        self.dac_min = 255;
        self.dac_max = 0;
        // Semilla del hash de los bytes DAC de ESTE frame (namespaced FM/ch5/DAC).
        self.dac_frame_hash = mix(mix(mix(FNV_SEED, CHIP_FM), 5), 0xDA);
        for w in writes {
            match w.chip {
                CHIP_FM => self.apply_fm(w.addr, w.data),
                CHIP_PSG => self.apply_psg(w.data),
                _ => {}
            }
        }
        // DAC (FM6, ch 5) segmentado por SILENCIO. "Con señal" = habilitado + hubo data
        // + las muestras varían (no es silencio constante). Abre al primer frame con
        // señal; cierra tras DAC_GAP_FRAMES de silencio, terminando en el último frame
        // con señal (la cola de silencio no entra al bloque).
        let loud = self.dac_enabled
            && self.dac_wrote
            && self.dac_max.saturating_sub(self.dac_min) >= DAC_SILENCE_RANGE;
        if loud {
            if !self.dac_on {
                self.dac_on = true;
                self.dac_start = frame;
                self.dac_sig = self.dac_frame_hash;
            }
            self.dac_last_loud = frame;
            self.dac_silence = 0;
        } else if self.dac_on {
            self.dac_silence += 1;
            if self.dac_silence >= DAC_GAP_FRAMES {
                self.dac_on = false;
                self.push_event(
                    self.dac_sig,
                    self.dac_sig,
                    CHIP_FM,
                    5,
                    self.dac_start,
                    self.dac_last_loud,
                    NO_PITCH,
                    NO_VELOCITY,
                );
            }
        }
        for e in pcm {
            self.apply_pcm(e);
        }
    }

    /// Closes channels still active at the end of a capture.
    ///
    /// The latest processed frame is used as an implicit key-off. Call once after
    /// the final [`Self::process_frame`] or [`Self::process_frame_ex`].
    pub fn finish(&mut self) {
        for ch in 0..6 {
            if self.fm_on[ch] {
                self.fm_on[ch] = false;
                self.push_event(
                    self.fm_sig[ch],
                    self.fm_inst[ch],
                    CHIP_FM,
                    ch as u8,
                    self.fm_start[ch],
                    self.frame,
                    self.fm_pitch[ch],
                    self.fm_vel[ch],
                );
            }
        }
        for ch in 0..4 {
            if self.psg_on[ch] {
                self.psg_on[ch] = false;
                self.push_event(
                    self.psg_sig[ch],
                    self.psg_inst[ch],
                    CHIP_PSG,
                    ch as u8,
                    self.psg_start[ch],
                    self.frame,
                    self.psg_pitch[ch],
                    self.psg_vel[ch],
                );
            }
        }
        if self.dac_on {
            self.dac_on = false;
            self.push_event(
                self.dac_sig,
                self.dac_sig,
                CHIP_FM,
                5,
                self.dac_start,
                self.dac_last_loud,
                NO_PITCH,
                NO_VELOCITY,
            );
        }
        for ch in 0..8 {
            if self.pcm_on[ch] {
                self.pcm_on[ch] = false;
                self.push_event(
                    self.pcm_sig[ch],
                    self.pcm_inst[ch],
                    CHIP_PCM,
                    ch as u8,
                    self.pcm_start[ch],
                    self.frame,
                    self.pcm_pitch[ch],
                    self.pcm_vel[ch],
                );
            }
        }
    }

    /// Returns all completed events accumulated by the detector.
    pub fn events(&self) -> &[AudioEvent] {
        &self.events
    }
    /// Returns the number of completed events.
    pub fn event_count(&self) -> usize {
        self.events.len()
    }

    /// Clears completed events without changing live channel state.
    ///
    /// This supports live runtime use where only currently active channels matter.
    pub fn clear_events(&mut self) {
        self.events.clear();
    }

    /// Returns channels that are currently keyed on, including their signatures.
    ///
    /// Runtime substitution uses this snapshot to mute mapped source channels and
    /// trigger the replacement for channels that have just become active.
    pub fn active_channels(&self) -> Vec<ActiveChannel> {
        let mut v = Vec::new();
        for ch in 0..6 {
            if self.fm_on[ch] {
                v.push(ActiveChannel {
                    signature: self.fm_sig[ch],
                    instrument: self.fm_inst[ch],
                    chip: CHIP_FM,
                    channel: ch as u8,
                    pitch: self.fm_pitch[ch],
                });
            }
        }
        for ch in 0..4 {
            if self.psg_on[ch] {
                v.push(ActiveChannel {
                    signature: self.psg_sig[ch],
                    instrument: self.psg_inst[ch],
                    chip: CHIP_PSG,
                    channel: ch as u8,
                    pitch: self.psg_pitch[ch],
                });
            }
        }
        if self.dac_on {
            // Mismo criterio que push_event: la identidad del DAC es su firma.
            v.push(ActiveChannel {
                signature: self.dac_sig,
                instrument: self.dac_sig,
                chip: CHIP_FM,
                channel: 5,
                pitch: NO_PITCH,
            });
        }
        for ch in 0..8 {
            if self.pcm_on[ch] {
                v.push(ActiveChannel {
                    signature: self.pcm_sig[ch],
                    instrument: self.pcm_inst[ch],
                    chip: CHIP_PCM,
                    channel: ch as u8,
                    pitch: self.pcm_pitch[ch],
                });
            }
        }
        v
    }

    // -- FM -----------------------------------------------------------------

    /// `addr` es el REGISTRO YA LATCHEADO (0x000-0x1FF), no el puerto del bus.
    ///
    /// Lo era hasta el fork `3fc6ee89` (2026-08-11), que consolidó la telemetría:
    /// antes el core mandaba el byte crudo del bus con su índice de puerto (0-3)
    /// y el latch lo replicaba el host; ahora el propio probe mantiene el shadow
    /// y entrega el registro resuelto. Seguir haciendo `addr & 3` interpretaba
    /// cada registro como un puerto —0x28 caía en «address port 0»— y el
    /// detector no veía UN SOLO evento: sin identidad, sin análisis de tomas y
    /// sin sustitución (2026-08-13, Golden Axe: 2130 frames → 0 eventos).
    fn apply_fm(&mut self, addr: u16, data: u8) {
        let reg = (addr & 0x1FF) as usize;
        self.fm_regs[reg] = data;
        // El banco 1 (0x100+) no tiene modo: key-on y DAC viven sólo en el 0.
        match reg {
            0x28 => self.fm_key(data), // key on/off (sólo banco 0)
            0x2A => {
                // DAC data (stream PCM)
                self.dac_wrote = true;
                if data < self.dac_min {
                    self.dac_min = data;
                }
                if data > self.dac_max {
                    self.dac_max = data;
                }
                self.dac_frame_hash = mix(self.dac_frame_hash, data);
            }
            0x2B => self.dac_enabled = (data & 0x80) != 0, // DAC enable (bit7)
            _ => {}
        }
    }

    /// Selects the clock region used to convert frequency registers to pitch.
    ///
    /// `false` selects NTSC and is the default; `true` selects PAL.
    pub fn set_pal(&mut self, pal: bool) {
        self.pal = pal;
    }

    /// Supplies evidence of channels already sounding at capture start.
    ///
    /// Bits 0–5 represent FM channels and bits 6–9 represent PSG channels. The
    /// default zero mask disables residual-event synthesis.
    pub fn set_initial_active(&mut self, mask: u16) {
        self.initial_active = mask;
    }

    /// Hz → nota MIDI redondeada (NO_PITCH fuera de 0-127 o sin frecuencia).
    fn midi_from_hz(f: f64) -> u8 {
        if !f.is_finite() || f <= 0.0 {
            return NO_PITCH;
        }
        let n = (69.0 + 12.0 * (f / 440.0).log2()).round();
        if !(0.0..=127.0).contains(&n) {
            NO_PITCH
        } else {
            n as u8
        }
    }

    /// Pitch FM del canal AHORA: fnum/block de 0xA0/0xA4 del shadow.
    /// freq = fnum × 2^(block−1) × fclk / (144 × 2^20)  (YM2612).
    fn fm_pitch_now(&self, ch: usize) -> u8 {
        let bank = if ch < 3 { 0 } else { 0x100 };
        let idx = ch % 3;
        let a4 = self.fm_regs[bank + 0xA4 + idx];
        let fnum = (((a4 & 0x07) as u32) << 8) | self.fm_regs[bank + 0xA0 + idx] as u32;
        if fnum == 0 {
            return NO_PITCH;
        }
        let block = (a4 >> 3) & 7;
        let clk = if self.pal { 7_600_489.0 } else { 7_670_453.0 };
        let freq = fnum as f64 * ((1u32 << block) as f64 / 2.0) * clk / (144.0 * 1_048_576.0);
        Self::midi_from_hz(freq)
    }

    /// Pitch PSG del canal AHORA: tono 10-bit (ch3 = ruido, sin pitch).
    /// f = clk / (32 × n)  (SN76489; n == 0 cuenta como 1024).
    fn psg_pitch_now(&self, ch: usize) -> u8 {
        if ch >= 3 {
            return NO_PITCH;
        }
        let n = match self.psg_freq[ch] & 0x3FF {
            0 => 1024,
            v => v as u32,
        };
        let clk = if self.pal { 3_546_895.0 } else { 3_579_545.0 };
        Self::midi_from_hz(clk / (32.0 * n as f64))
    }

    fn fm_key(&mut self, data: u8) {
        let lo = data & 0x03;
        if lo == 3 {
            return;
        } // canal inválido
        let ch = (lo + if data & 0x04 != 0 { 3 } else { 0 }) as usize;
        if ch == 5 && self.dac_enabled {
            return;
        } // FM6 en modo DAC → lo maneja el DAC
        let on = (data & 0xF0) != 0; // algún operador encendido
        if on && !self.fm_on[ch] {
            self.fm_on[ch] = true;
            self.fm_seen[ch] = true;
            self.fm_start[ch] = self.frame;
            self.fm_sig[ch] = self.fm_signature(ch);
            self.fm_inst[ch] = self.fm_instrument(ch);
            self.fm_vel[ch] = self.fm_velocity(ch);
            self.fm_pitch[ch] = self.fm_pitch_now(ch);
        } else if !on
            && !self.fm_on[ch]
            && !self.fm_seen[ch]
            && self.frame > 0
            && self.initial_active & (1u16 << ch) != 0
        {
            // Key-off SIN key-on previo esta toma: la nota venía sonando desde
            // el estado inicial → evento RESIDUAL [0, off-1] (firma/pitch del
            // shadow actual; si los regs de frecuencia no se escribieron en la
            // toma, el pitch cae a NO_PITCH — mejor un bloque sin altura que un
            // sonido invisible).
            self.fm_seen[ch] = true;
            let end = self.frame - 1;
            let sig = self.fm_signature(ch);
            let inst = self.fm_instrument(ch);
            let pitch = self.fm_pitch_now(ch);
            self.push_event(
                sig,
                inst,
                CHIP_FM,
                ch as u8,
                0,
                end,
                pitch,
                self.fm_velocity(ch),
            );
        } else if !on && self.fm_on[ch] {
            self.fm_on[ch] = false;
            // El evento abarca los frames que SONÓ: [key-on, key-off-1]. Así dos notas
            // contiguas (key-off de una = key-on de la otra) NO comparten frame → no se
            // pisan al dibujar ni se apilan en niveles distintos en la Secuencia.
            let end = self.frame.saturating_sub(1).max(self.fm_start[ch]);
            self.push_event(
                self.fm_sig[ch],
                self.fm_inst[ch],
                CHIP_FM,
                ch as u8,
                self.fm_start[ch],
                end,
                self.fm_pitch[ch],
                self.fm_vel[ch],
            );
        }
    }

    /// Firma del canal FM: hash del "patch" (4 operadores) + registros de
    /// frecuencia/algoritmo del canal, en el momento del key-on. Estable para el
    /// mismo SFX (los mismos parámetros de instrumento + nota).
    fn fm_signature(&self, ch: usize) -> u64 {
        let bank = if ch < 3 { 0 } else { 0x100 };
        let idx = ch % 3;
        let mut h = mix(mix(FNV_SEED, CHIP_FM), ch as u8);
        // Registros de operador del canal: 0x30,0x34,…,0x9C con (reg & 3) == idx.
        let mut base = 0x30;
        while base <= 0x9C {
            h = mix(h, self.fm_regs[bank + base + idx]);
            base += 4;
        }
        // Registros por canal: frecuencia (0xA0/0xA4), FB/algoritmo (0xB0), L/R/AMS/FMS (0xB4).
        for off in [0xA0usize, 0xA4, 0xB0, 0xB4] {
            h = mix(h, self.fm_regs[bank + off + idx]);
        }
        h
    }

    /// Instrumento del canal FM: como `fm_signature` pero SIN la frecuencia
    /// (0xA0/0xA4), SIN el canal en la semilla (la rotación del driver mueve la
    /// misma voz entre canales), SIN los bits de pan L/R de 0xB4 (el mismo
    /// patch panneado distinto es el mismo instrumento) y SIN el Total Level de
    /// los operadores PORTADORES. 0xF1 = tag namespaced.
    ///
    /// EL TL DEL PORTADOR ES EL VOLUMEN, NO EL TIMBRE. El de un modulador sí es
    /// timbre —es el índice de modulación— y se queda. Incluir el del portador
    /// hacía que un fundido o un acento cambiaran la identidad, fragmentando un
    /// instrumento en varios.
    ///
    /// MEDIDO sobre 60 s de Sonic: 43 identidades donde
    /// hay 30, y en FM0 un solo instrumento partido en dieciséis.
    ///
    /// OJO: desde 2026-08-10 el instrument YA NO es efímero — las
    /// reglas de match por instrumento lo PERSISTEN en audio_events.toml
    /// (`match = "instrument"` + `instrument = "0x…"`). Cambiar este hash
    /// invalida esa autoría (habría que re-autorarla); es exactamente lo que
    /// pasó con el hash de sprite y el flip.
    ///
    /// El volumen no se pierde: sale por `velocity` en el evento.
    fn fm_instrument(&self, ch: usize) -> u64 {
        let bank = if ch < 3 { 0 } else { 0x100 };
        let idx = ch % 3;
        let carriers = FM_CARRIERS[(self.fm_regs[bank + 0xB0 + idx] & 7) as usize];
        let mut h = mix(mix(FNV_SEED, CHIP_FM), 0xF1);
        let mut base = 0x30;
        while base <= 0x9C {
            // 0x40 = Total Level. Se salta sólo en los portadores.
            let op = (base - 0x30) / 4; // 0x30,0x34,0x38,0x3C → op 0..3
            let is_tl = (base & 0xF0) == 0x40;
            if !(is_tl && carriers & (1 << (op % 4)) != 0) {
                h = mix(h, self.fm_regs[bank + base + idx]);
            }
            base += 4;
        }
        h = mix(h, self.fm_regs[bank + 0xB0 + idx]); // FB/algoritmo
        h = mix(h, self.fm_regs[bank + 0xB4 + idx] & 0x3F); // AMS/FMS, sin pan
        h
    }

    /// «Velocidad» del canal FM: el Total Level del operador PORTADOR más
    /// fuerte, invertido a escala MIDI. En FM no existe velocity — esto es lo
    /// más parecido, y es la mitad de la información que `fm_instrument` deja
    /// deliberadamente afuera.
    fn fm_velocity(&self, ch: usize) -> u8 {
        let bank = if ch < 3 { 0 } else { 0x100 };
        let idx = ch % 3;
        let carriers = FM_CARRIERS[(self.fm_regs[bank + 0xB0 + idx] & 7) as usize];
        let mut best = 127u8;
        for op in 0..4usize {
            if carriers & (1 << op) == 0 {
                continue;
            }
            let tl = self.fm_regs[bank + 0x40 + op * 4 + idx] & 0x7F;
            if tl < best {
                best = tl;
            } // menor TL = más fuerte
        }
        let v = 127i32 - best as i32; // TL 0 = máximo, 127 = silencio
        if v < 1 { 1 } else { v as u8 }
    }

    // -- PSG ----------------------------------------------------------------

    fn apply_psg(&mut self, data: u8) {
        if data & 0x80 != 0 {
            // Byte de latch: 1 rrr dddd  (rrr = registro 0-7, dddd = nibble bajo).
            let index = ((data >> 4) & 0x07) as usize;
            self.psg_latch = index as u8;
            let nib = data & 0x0F;
            if index & 1 == 1 {
                // Atenuación (índices 1,3,5,7 → canales 0-3). 0xF = mudo.
                self.psg_atten(index >> 1, nib);
            } else if index == 6 {
                // Control de ruido (canal 3): guardar para la firma.
                self.psg_freq[3] = nib as u16;
            } else {
                // Frecuencia de tono LSB (índices 0,2,4 → canales 0-2).
                let ch = index >> 1;
                self.psg_freq[ch] = (self.psg_freq[ch] & 0x3F0) | (nib as u16);
            }
        } else {
            // Byte de dato al registro latcheado: MSB de frecuencia de tono.
            let index = self.psg_latch as usize;
            if index & 1 == 0 && index <= 4 {
                let ch = index >> 1;
                self.psg_freq[ch] = (self.psg_freq[ch] & 0x00F) | (((data & 0x3F) as u16) << 4);
            }
        }
    }

    fn psg_atten(&mut self, ch: usize, atten: u8) {
        let on = atten != 0x0F;
        if on && !self.psg_on[ch] {
            self.psg_on[ch] = true;
            self.psg_seen[ch] = true;
            self.psg_start[ch] = self.frame;
            self.psg_sig[ch] = self.psg_signature(ch);
            self.psg_inst[ch] = self.psg_instrument(ch);
            self.psg_pitch[ch] = self.psg_pitch_now(ch);
            self.psg_vel[ch] = psg_velocity(atten);
        } else if !on
            && !self.psg_on[ch]
            && !self.psg_seen[ch]
            && self.frame > 0
            && self.initial_active & (1u16 << (6 + ch)) != 0
        {
            // Ídem FM: mute sin encendido previo = sonido del estado inicial.
            self.psg_seen[ch] = true;
            let end = self.frame - 1;
            let sig = self.psg_signature(ch);
            let inst = self.psg_instrument(ch);
            let pitch = self.psg_pitch_now(ch);
            self.push_event(sig, inst, CHIP_PSG, ch as u8, 0, end, pitch, NO_VELOCITY);
        } else if !on && self.psg_on[ch] {
            self.psg_on[ch] = false;
            let end = self.frame.saturating_sub(1).max(self.psg_start[ch]);
            self.push_event(
                self.psg_sig[ch],
                self.psg_inst[ch],
                CHIP_PSG,
                ch as u8,
                self.psg_start[ch],
                end,
                self.psg_pitch[ch],
                self.psg_vel[ch],
            );
        }
    }

    /// Firma del canal PSG: hash de (canal, frecuencia/control) al key-on.
    fn psg_signature(&self, ch: usize) -> u64 {
        let f = self.psg_freq[ch];
        let mut h = mix(mix(FNV_SEED, CHIP_PSG), ch as u8);
        h = mix(h, (f & 0xFF) as u8);
        h = mix(h, (f >> 8) as u8);
        h
    }

    /// Instrumento del canal PSG: los canales de tono (0-2) son la MISMA onda
    /// cuadrada sin timbre → una constante compartida; el ruido (ch3) se
    /// distingue por su registro de control (white/periodic + rate). 0xF0 = tag.
    fn psg_instrument(&self, ch: usize) -> u64 {
        let h = mix(mix(FNV_SEED, CHIP_PSG), 0xF0);
        if ch < 3 {
            h
        } else {
            mix(h, (self.psg_freq[3] & 0x0F) as u8)
        }
    }

    // -- PCM (RF5C164, Sega CD) ---------------------------------------------

    fn apply_pcm(&mut self, e: &PcmEvent) {
        let ch = e.channel as usize;
        if ch >= 8 {
            return;
        }
        match e.kind {
            PCM_KEY_ON => {
                // Un re-disparo sobre un canal que ya sonaba CIERRA el bloque
                // anterior y abre uno nuevo: el driver reapunta el canal a otro
                // sample sin apagarlo, y sin esto los dos sonidos quedarían
                // dentro de un mismo bloque.
                if self.pcm_on[ch] {
                    let end = self.frame.saturating_sub(1).max(self.pcm_start[ch]);
                    self.push_event(
                        self.pcm_sig[ch],
                        self.pcm_inst[ch],
                        CHIP_PCM,
                        ch as u8,
                        self.pcm_start[ch],
                        end,
                        self.pcm_pitch[ch],
                        self.pcm_vel[ch],
                    );
                }
                self.pcm_on[ch] = true;
                self.pcm_start[ch] = self.frame;
                self.pcm_sig[ch] = Self::pcm_signature(ch, e);
                self.pcm_inst[ch] = Self::pcm_instrument(e);
                self.pcm_vel[ch] = pcm_velocity(e.env);
                self.pcm_pitch[ch] = pcm_pitch(e.fd);
            }
            PCM_KEY_OFF if self.pcm_on[ch] => {
                self.pcm_on[ch] = false;
                // Mismo criterio que FM/PSG: el bloque abarca [key-on, off-1].
                let end = self.frame.saturating_sub(1).max(self.pcm_start[ch]);
                self.push_event(
                    self.pcm_sig[ch],
                    self.pcm_inst[ch],
                    CHIP_PCM,
                    ch as u8,
                    self.pcm_start[ch],
                    end,
                    self.pcm_pitch[ch],
                    self.pcm_vel[ch],
                );
            }
            // VOLUME y PITCH no abren ni cierran nada, y a propósito no tocan la
            // firma ya capturada: la identidad es la del MOMENTO DEL DISPARO,
            // igual que en FM (un fundido no cambia de qué sonido se trata).
            _ => {}
        }
    }

    /// Firma del canal PCM: qué sample (st/ls) a qué velocidad (fd), con el
    /// canal en la semilla igual que en FM y PSG.
    fn pcm_signature(ch: usize, e: &PcmEvent) -> u64 {
        let mut h = mix(mix(FNV_SEED, CHIP_PCM), ch as u8);
        h = mix(h, e.st);
        h = mix(h, (e.ls & 0xFF) as u8);
        h = mix(h, (e.ls >> 8) as u8);
        h = mix(h, (e.fd & 0xFF) as u8);
        h = mix(h, (e.fd >> 8) as u8);
        h
    }

    /// Instrumento del canal PCM: SÓLO el sample (st + ls). Sin canal —el
    /// driver rota canales como en FM—, sin `fd` —la velocidad es la nota— y
    /// sin `env`/`pan` —volumen y paneo no son identidad de timbre—.
    /// 0xF2 = tag namespaced, hermano del 0xF1 de FM y del 0xF0 de PSG.
    fn pcm_instrument(e: &PcmEvent) -> u64 {
        let mut h = mix(mix(FNV_SEED, CHIP_PCM), 0xF2);
        h = mix(h, e.st);
        h = mix(h, (e.ls & 0xFF) as u8);
        h = mix(h, (e.ls >> 8) as u8);
        h
    }

    // -- común --------------------------------------------------------------

    #[expect(
        clippy::too_many_arguments,
        reason = "the parameters map directly to the flat AudioEvent record at numerous call sites"
    )]
    fn push_event(
        &mut self,
        signature: u64,
        instrument: u64,
        chip: u8,
        channel: u8,
        start: u32,
        end: u32,
        pitch: u8,
        velocity: u8,
    ) {
        self.events.push(AudioEvent {
            signature,
            instrument,
            chip,
            channel,
            start_frame: start,
            end_frame: end,
            pitch,
            velocity,
        });
    }
}

// ---------------------------------------------------------------------------
// Persistencia: audio_events.toml  (C-A5)
//
// Catálogo de sustituciones de audio por evento: FIRMA → asset HD (+ máscara de
// canales involucrados, para que el runtime sepa qué silenciar sin re-analizar).
// Mismo `[[event]]`/hex-string que el resto de los TOML del.ay. Sirve para
// guardar/cargar el proyecto y para la entrega. Los EVENTOS en sí no se guardan
// (son re-derivables re-analizando la toma, deterministas); sólo las asignaciones.
// ---------------------------------------------------------------------------

/// Persistable HD substitution for a detected audio event.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EventSub {
    /// Exact trigger signature used as the persistent catalog key.
    pub signature: u64,
    /// Logical replacement asset path inside the pack.
    pub asset: String,
    /// Union of source channels observed for this event signature.
    ///
    /// Bits 0–5 represent YM2612 FM channels, bits 6–9 represent SN76489 PSG
    /// channels, and bits 10–17 represent Sega CD RF5C164 PCM channels. Zero means
    /// unknown and can be reconstructed by reanalyzing the capture.
    pub channels: u32,
    /// Relative replacement window in frames after the trigger.
    ///
    /// During the window the source `channels` remain muted and the replacement
    /// plays. Zero selects classic per-event behavior, which mutes only while the
    /// trigger signature is active.
    pub duration_frames: u32,
    /// Whether the replacement loops until the duration window closes.
    pub looping: bool,
    /// Matching rule for this assignment.
    ///
    /// [`MATCH_EXACT`] matches only the signature, [`MATCH_INSTRUMENT`] matches
    /// any voice with the same instrument, and [`MATCH_INSTRUMENT_PITCH`] also
    /// requires the same note. `signature` remains the persistent catalog key.
    pub match_rule: u8,
    /// Authored instrument identity used by the selected rule.
    ///
    /// It is persisted to avoid recomputing authoring intent during playback and
    /// is zero for [`MATCH_EXACT`].
    pub match_instrument: u64,
    /// MIDI note for [`MATCH_INSTRUMENT_PITCH`], or [`NO_PITCH`] when unavailable.
    pub match_pitch: u8,
    /// Audio bus for the replacement: 0 unclassified, 1 music, 2 effects, or 3
    /// voices.
    ///
    /// Missing values remain unclassified rather than being interpreted as
    /// effects, preserving the meaning of packs that predate bus metadata.
    pub bus: u8,
}

/// Match only the exact event signature.
pub const MATCH_EXACT: u8 = 0;
/// Match any event with the authored instrument identity.
pub const MATCH_INSTRUMENT: u8 = 1;
/// Match the authored instrument identity and MIDI pitch.
pub const MATCH_INSTRUMENT_PITCH: u8 = 2;

/// Serializes an event-substitution catalog as `audio_events.toml`.
pub fn events_to_toml(subs: &[EventSub]) -> String {
    let mut s = String::new();
    s.push_str("# audio_events.toml — event-based audio substitutions\n");
    s.push_str(
        "# signature identifies the chip-command event; channels is the involved-channel mask\n",
    );
    s.push_str("# (bits 0-5 FM, 6-9 PSG, and 10-17 PCM).\n\n");
    for e in subs {
        s.push_str("[[event]]\n");
        s.push_str(&format!("signature = \"0x{:016x}\"\n", e.signature));
        // escapar comillas/backslash del asset por las dudas
        let asset = e.asset.replace('\\', "\\\\").replace('"', "\\\"");
        s.push_str(&format!("asset = \"{}\"\n", asset));
        s.push_str(&format!("channels = \"0x{:08x}\"\n", e.channels));
        // Campos de SECUENCIA — omitidos en la sustitución per-evento clásica
        // (un pack viejo y uno sin secuencias son byte-idénticos al formato previo).
        if e.duration_frames != 0 {
            s.push_str(&format!("duration = {}\n", e.duration_frames));
        }
        if e.looping {
            s.push_str("loop = true\n");
        }
        //  el bus, sólo si fue clasificado. Mismo criterio que el resto de
        // los opcionales: un pack sin buses queda byte-idéntico al formato
        // previo, y «sin clasificar» se lee como lo que es — nadie lo dijo.
        if e.bus != 0 {
            s.push_str(&format!(
                "bus = \"{}\"\n",
                match e.bus {
                    1 => "music",
                    2 => "sfx",
                    _ => "voice",
                }
            ));
        }
        // F3: la regla viaja sólo si fue autorada Y trae su identidad —
        // una entrada legacy/exacta sigue siendo byte-idéntica al formato previo.
        if e.match_rule != MATCH_EXACT && e.match_instrument != 0 {
            s.push_str(&format!(
                "match = \"{}\"\n",
                if e.match_rule == MATCH_INSTRUMENT_PITCH {
                    "instrument_pitch"
                } else {
                    "instrument"
                }
            ));
            s.push_str(&format!("instrument = \"0x{:016x}\"\n", e.match_instrument));
            if e.match_rule == MATCH_INSTRUMENT_PITCH {
                s.push_str(&format!("pitch = {}\n", e.match_pitch));
            }
        }
        s.push('\n');
    }
    s
}

/// Parses an `audio_events.toml` catalog and ignores malformed entries.
pub fn events_from_toml(text: &str) -> Vec<EventSub> {
    let mut out = Vec::new();
    let tbl: toml::Value = match toml::from_str(text) {
        Ok(t) => t,
        Err(_) => return out,
    };
    let arr = match tbl.get("event").and_then(|v| v.as_array()) {
        Some(a) => a,
        None => return out,
    };
    for e in arr {
        let sig = match e
            .get("signature")
            .and_then(|v| v.as_str())
            .and_then(parse_hex_u64)
        {
            Some(s) => s,
            None => continue,
        };
        let asset = match e.get("asset").and_then(|v| v.as_str()) {
            Some(a) => a.to_string(),
            None => continue,
        };
        let channels = e
            .get("channels")
            .and_then(|v| v.as_str())
            .and_then(parse_hex_u64)
            .unwrap_or(0) as u32;
        // Defaults tolerantes: un TOML viejo (sin duration/loop) parsea igual.
        let duration_frames = e
            .get("duration")
            .and_then(|v| v.as_integer())
            .unwrap_or(0)
            .max(0) as u32;
        let looping = e.get("loop").and_then(|v| v.as_bool()).unwrap_or(false);
        // F3: regla de match — ausente o desconocida = exacta (legacy).
        // Una regla SIN instrumento no puede armarse → cae a exacta.
        let match_instrument = e
            .get("instrument")
            .and_then(|v| v.as_str())
            .and_then(parse_hex_u64)
            .unwrap_or(0);
        let match_rule = match e.get("match").and_then(|v| v.as_str()) {
            Some("instrument") if match_instrument != 0 => MATCH_INSTRUMENT,
            Some("instrument_pitch") if match_instrument != 0 => MATCH_INSTRUMENT_PITCH,
            _ => MATCH_EXACT,
        };
        let match_pitch = e
            .get("pitch")
            .and_then(|v| v.as_integer())
            .filter(|_| match_rule == MATCH_INSTRUMENT_PITCH)
            .map(|p| p.clamp(0, 255) as u8)
            .unwrap_or(255);
        //  un nombre desconocido cae a 0 (sin clasificar) y no a Efectos:
        // un pack de mañana puede traer un bus nuevo, y meterlo en Efectos lo
        // haría bajar con un slider que no es el suyo.
        let bus = match e.get("bus").and_then(|v| v.as_str()) {
            Some("music") => 1u8,
            Some("sfx") => 2u8,
            Some("voice") => 3u8,
            _ => 0u8,
        };
        out.push(EventSub {
            signature: sig,
            asset,
            channels,
            duration_frames,
            looping,
            match_rule,
            match_instrument: if match_rule == MATCH_EXACT {
                0
            } else {
                match_instrument
            },
            match_pitch,
            bus,
        });
    }
    out
}

/// "0x...." | "...." → u64. None si malformado.
fn parse_hex_u64(s: &str) -> Option<u64> {
    let t = s.trim();
    let t = t
        .strip_prefix("0x")
        .or_else(|| t.strip_prefix("0X"))
        .unwrap_or(t);
    u64::from_str_radix(t, 16).ok()
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn fm(addr: u16, data: u8) -> AudioWrite {
        AudioWrite {
            cycle: 0,
            addr,
            data,
            chip: CHIP_FM,
        }
    }
    fn psg(data: u8) -> AudioWrite {
        AudioWrite {
            cycle: 0,
            data,
            addr: 0,
            chip: CHIP_PSG,
        }
    }

    // Escribe un registro FM. UNA escritura: el core entrega el registro ya
    // latcheado (banco 1 = 0x100+), no el par puerto-dirección / puerto-dato.
    fn fm_write(addr: u16, data: u8) -> [AudioWrite; 1] {
        [fm(addr, data)]
    }
    // Key-on del canal `ch` (operadores todos on) y key-off, vía reg 0x28.
    fn fm_keyon(ch: u8) -> u8 {
        0xF0 | (ch % 3) | if ch >= 3 { 0x04 } else { 0 }
    }
    fn fm_keyoff(ch: u8) -> u8 {
        (ch % 3) | if ch >= 3 { 0x04 } else { 0 }
    }

    #[test]
    fn pitch_decodes_known_notes() {
        // FM ch0: fnum 1083 · block 4 → ≈440.1 Hz = A4 (nota MIDI 69).
        // freq = fnum × 2^(block−1) × fclk / (144 × 2^20), NTSC 7 670 453 Hz.
        let mut d = AudioEventDetector::new();
        let mut w = Vec::new();
        w.extend(fm_write(0xA4, 0x24)); // block 4 (bits 3-5) + fnum hi 0x4
        w.extend(fm_write(0xA0, 0x3B)); // fnum lo (1083 = 0x43B)
        w.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(1, &w);
        d.process_frame(2, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(d.events()[0].pitch, 69, "FM fnum 1083 / block 4 = A4");

        // PSG ch0: n = 254 → 3579545/(32×254) ≈ 440.4 Hz = A4.
        let p = |data: u8| AudioWrite {
            cycle: 0,
            addr: 0,
            data,
            chip: CHIP_PSG,
        };
        let mut q = AudioEventDetector::new();
        // latch tono ch0 (reg 0, nibble bajo 0xE) + data MSB (254 >> 4 = 15).
        q.process_frame(1, &[p(0x80 | 0x0E), p(0x0F), p(0x90)]); // atten 0 = on
        q.process_frame(2, &[p(0x9F)]); // atten F = off
        assert_eq!(q.events()[0].pitch, 69, "PSG n = 254 = A4");
    }

    #[test]
    fn keyoff_sin_keyon_sintetiza_evento_residual() {
        // FM: nota que venía sonando desde el ESTADO INICIAL de la toma — el
        // primer comando que llega del canal es el key-OFF. Debe sintetizarse
        // un evento residual [0, off-1] (con frecuencia escrita en la toma, el
        // pitch se decodifica del shadow).
        let mut d = AudioEventDetector::new();
        d.set_initial_active(1 << 0); // la sonda de PCM oyó FM0 al inicio
        let mut w = Vec::new();
        w.extend(fm_write(0xA4, 0x24)); // la toma escribe frecuencia antes del off
        w.extend(fm_write(0xA0, 0x3B));
        w.extend([fm(0x28, fm_keyoff(0))]);
        d.process_frame(30, &w);
        assert_eq!(d.events().len(), 1, "el key-off huérfano emite el residual");
        let e = d.events()[0];
        assert_eq!((e.chip, e.channel), (CHIP_FM, 0));
        assert_eq!((e.start_frame, e.end_frame), (0, 29), "residual [0, off-1]");
        assert_eq!(e.pitch, 69, "pitch del shadow (A4)");
        // Un SEGUNDO key-off huérfano del mismo canal NO re-emite (one-shot).
        d.process_frame(40, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(d.events().len(), 1, "el residual es one-shot por canal");

        // FM sin EVIDENCIA de audio (la sonda no oyó el canal): nada, aunque
        // la toma haya escrito registros del canal.
        let mut d0 = AudioEventDetector::new();
        let mut w0 = Vec::new();
        w0.extend(fm_write(0xA4, 0x24));
        w0.extend(fm_write(0xA0, 0x3B));
        w0.extend([fm(0x28, fm_keyoff(0))]);
        d0.process_frame(30, &w0);
        assert_eq!(
            d0.events().len(),
            0,
            "off de init sin evidencia de AUDIO = sin residual"
        );

        // PSG: el mute de INIT sin evidencia de audio NO emite (los drivers
        // mutean canales silenciosos — y hasta les escriben frecuencia, GA lo
        // hace — al inicializar la música).
        let p = |data: u8| AudioWrite {
            cycle: 0,
            addr: 0,
            data,
            chip: CHIP_PSG,
        };
        let mut q0 = AudioEventDetector::new();
        q0.process_frame(10, &[p(0x80 | 0x0E), p(0x0F)]);
        q0.process_frame(20, &[p(0x9F)]);
        assert_eq!(
            q0.events().len(),
            0,
            "mute de init sin evidencia de audio = sin residual"
        );
        // Con la sonda de PCM oyendo el canal, SÍ es residual.
        let mut q = AudioEventDetector::new();
        q.set_initial_active(1 << 6); // PSG0 sonaba
        q.process_frame(10, &[p(0x80 | 0x0E), p(0x0F)]);
        q.process_frame(20, &[p(0x9F)]);
        assert_eq!(q.events().len(), 1);
        assert_eq!(
            (q.events()[0].start_frame, q.events()[0].end_frame),
            (0, 19)
        );

        // Y el flujo normal NO cambia: key-on en frame 0 → sin residual espurio.
        let mut n = AudioEventDetector::new();
        n.process_frame(0, &[fm(0x28, fm_keyon(0))]);
        n.process_frame(5, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(n.events().len(), 1);
        assert_eq!(n.events()[0].start_frame, 0);
    }

    #[test]
    fn pitch_no_pitch_and_pal() {
        // fnum 0 → NO_PITCH (canal sin frecuencia programada).
        let mut d = AudioEventDetector::new();
        let mut w = Vec::new();
        w.extend(fm_write(0xA4, 0x20));
        w.extend(fm_write(0xA0, 0x00));
        w.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(1, &w);
        d.process_frame(2, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(d.events()[0].pitch, NO_PITCH, "fnum 0 = sin altura");

        // PAL (≈ −0,16 semitonos): la MISMA nota tras redondear.
        let mut dp = AudioEventDetector::new();
        dp.set_pal(true);
        let mut wp = Vec::new();
        wp.extend(fm_write(0xA4, 0x24));
        wp.extend(fm_write(0xA0, 0x3B));
        wp.extend([fm(0x28, fm_keyon(0))]);
        dp.process_frame(1, &wp);
        dp.process_frame(2, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(dp.events()[0].pitch, 69, "PAL redondea a la misma nota");

        // El pitch se captura AL KEY-ON: cambiar la frecuencia después no lo
        // muta (la nota del evento es la del ataque).
        let mut da = AudioEventDetector::new();
        let mut wa = Vec::new();
        wa.extend(fm_write(0xA4, 0x24));
        wa.extend(fm_write(0xA0, 0x3B));
        wa.extend([fm(0x28, fm_keyon(0))]);
        da.process_frame(1, &wa);
        let mut wb = Vec::new();
        wb.extend(fm_write(0xA0, 0xFF)); // slide posterior
        da.process_frame(2, &wb);
        da.process_frame(3, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(da.events()[0].pitch, 69, "pitch = el del ataque");
    }

    #[test]
    fn fm_key_on_off_emits_one_event_with_frame_span() {
        let mut d = AudioEventDetector::new();
        // frame 10: configurar un patch + key-on canal 0
        let mut w = Vec::new();
        w.extend(fm_write(0xA0, 0x55)); // freq LSB
        w.extend(fm_write(0xA4, 0x22)); // freq MSB/block
        w.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(10, &w);
        assert_eq!(d.event_count(), 0, "todavía suena, no hay evento cerrado");
        // frame 25: key-off
        d.process_frame(25, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(d.event_count(), 1);
        let e = d.events()[0];
        assert_eq!(e.chip, CHIP_FM);
        assert_eq!(e.channel, 0);
        assert_eq!(e.start_frame, 10);
        assert_eq!(
            e.end_frame, 24,
            "abarca [key-on, key-off-1] (frames que sonó)"
        );
        assert_ne!(e.signature, 0);
    }

    #[test]
    fn contiguous_notes_dont_share_a_frame() {
        // Dos notas pegadas (key-off de la 1ª = key-on de la 2ª, mismo frame) en el
        // mismo canal NO deben compartir un frame de borde: ev1.end < ev2.start.
        let mut d = AudioEventDetector::new();
        d.process_frame(0, &[fm(0x28, fm_keyon(0))]);
        // frame 10: key-off de la 1ª seguido de key-on de la 2ª (misma nota, transición)
        d.process_frame(10, &[fm(0x28, fm_keyoff(0)), fm(0x28, fm_keyon(0))]);
        d.process_frame(20, &[fm(0x28, fm_keyoff(0))]);
        let ev = d.events();
        assert_eq!(ev.len(), 2);
        let a = ev.iter().find(|e| e.start_frame == 0).unwrap();
        let b = ev.iter().find(|e| e.start_frame == 10).unwrap();
        assert_eq!(a.end_frame, 9, "1ª termina en key-off-1");
        assert_eq!(b.start_frame, 10);
        assert!(a.end_frame < b.start_frame, "no comparten frame de borde");
    }

    #[test]
    fn same_patch_same_signature_different_patch_differs() {
        // Dos SFX idénticos (mismo patch+freq) en canales distintos → misma firma
        // de forma (salvo el canal, que entra en el hash). Verificamos que el MISMO
        // canal con el MISMO patch da la misma firma, y distinto patch da otra.
        let mut d = AudioEventDetector::new();

        let mut a = Vec::new();
        a.extend(fm_write(0xA0, 0x40));
        a.extend(fm_write(0x30, 0x71)); // DT/MUL operador
        a.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(0, &a);
        d.process_frame(1, &[fm(0x28, fm_keyoff(0))]);

        // mismo patch otra vez en el canal 0
        d.process_frame(2, &a);
        d.process_frame(3, &[fm(0x28, fm_keyoff(0))]);

        // patch distinto (cambiar MUL) en canal 0
        let mut b = Vec::new();
        b.extend(fm_write(0x30, 0x09));
        b.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(4, &b);
        d.process_frame(5, &[fm(0x28, fm_keyoff(0))]);

        let ev = d.events();
        assert_eq!(ev.len(), 3);
        assert_eq!(
            ev[0].signature, ev[1].signature,
            "mismo patch → misma firma"
        );
        assert_ne!(
            ev[0].signature, ev[2].signature,
            "patch distinto → firma distinta"
        );
    }

    #[test]
    fn instrument_ignora_frecuencia_y_distingue_patch() {
        // La misma voz tocando dos notas → firmas DISTINTAS, instrument IGUAL;
        // cambiar el patch (MUL) → instrument distinto.
        let mut d = AudioEventDetector::new();
        let mut a = Vec::new();
        a.extend(fm_write(0x30, 0x71)); // patch
        a.extend(fm_write(0xA0, 0x40)); // nota 1
        a.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(0, &a);
        d.process_frame(1, &[fm(0x28, fm_keyoff(0))]);

        let mut b = Vec::new();
        b.extend(fm_write(0xA0, 0x80)); // nota 2, mismo patch
        b.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(2, &b);
        d.process_frame(3, &[fm(0x28, fm_keyoff(0))]);

        let mut c = Vec::new();
        c.extend(fm_write(0x30, 0x09)); // patch distinto, misma nota 2
        c.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(4, &c);
        d.process_frame(5, &[fm(0x28, fm_keyoff(0))]);

        let ev = d.events();
        assert_eq!(ev.len(), 3);
        assert_ne!(
            ev[0].signature, ev[1].signature,
            "nota distinta → firma distinta"
        );
        assert_eq!(
            ev[0].instrument, ev[1].instrument,
            "misma voz → mismo instrument"
        );
        assert_ne!(
            ev[1].instrument, ev[2].instrument,
            "patch distinto → instrument distinto"
        );
    }

    #[test]
    fn instrument_igual_entre_canales() {
        // El mismo patch en FM0 y FM1 (rotación del driver): firmas distintas
        // (el canal entra en la firma) pero el MISMO instrument.
        let mut d = AudioEventDetector::new();
        let mut a = Vec::new();
        a.extend(fm_write(0x30, 0x71)); // patch ch0 (idx 0)
        a.extend(fm_write(0x31, 0x71)); // mismo patch ch1 (idx 1)
        a.extend(fm_write(0xA0, 0x40));
        a.extend(fm_write(0xA1, 0x40));
        a.extend([fm(0x28, fm_keyon(0)), fm(0x28, fm_keyon(1))]);
        d.process_frame(0, &a);
        d.process_frame(1, &[fm(0x28, fm_keyoff(0)), fm(0x28, fm_keyoff(1))]);
        let ev = d.events();
        assert_eq!(ev.len(), 2);
        assert_ne!(
            ev[0].signature, ev[1].signature,
            "el canal entra en la firma"
        );
        assert_eq!(
            ev[0].instrument, ev[1].instrument,
            "misma voz en otro canal"
        );
    }

    #[test]
    fn instrument_fm_ignora_pan() {
        // El mismo patch con pan L vs pan R (bits 6-7 de 0xB4) es el MISMO
        // instrumento (firma sí cambia).
        let mut d = AudioEventDetector::new();
        let mut a = Vec::new();
        a.extend(fm_write(0xB4, 0x80)); // pan L
        a.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(0, &a);
        d.process_frame(1, &[fm(0x28, fm_keyoff(0))]);
        let mut b = Vec::new();
        b.extend(fm_write(0xB4, 0x40)); // pan R
        b.extend([fm(0x28, fm_keyon(0))]);
        d.process_frame(2, &b);
        d.process_frame(3, &[fm(0x28, fm_keyoff(0))]);
        let ev = d.events();
        assert_eq!(ev.len(), 2);
        assert_ne!(ev[0].signature, ev[1].signature);
        assert_eq!(
            ev[0].instrument, ev[1].instrument,
            "el pan no parte el instrumento"
        );
    }

    #[test]
    fn psg_instrument_tono_constante_ruido_por_control() {
        // Tonos distintos en canales distintos → mismo instrument (cuadrada pura);
        // ruido → instrument propio, distinto por control de ruido.
        let mut d = AudioEventDetector::new();
        // ch0 tono n=0x0E, on/off (latch reg0 + atten reg1)
        d.process_frame(0, &[psg(0x80 | 0x0E), psg(0x90)]);
        d.process_frame(1, &[psg(0x9F)]);
        // ch1 tono n=0x05, on/off (latch reg2 + atten reg3)
        d.process_frame(2, &[psg(0xA0 | 0x05), psg(0xB0)]);
        d.process_frame(3, &[psg(0xBF)]);
        // ch3 ruido control 0x04, on/off (latch reg6 + atten reg7)
        d.process_frame(4, &[psg(0xE4), psg(0xF0)]);
        d.process_frame(5, &[psg(0xFF)]);
        // ch3 ruido control 0x07
        d.process_frame(6, &[psg(0xE7), psg(0xF0)]);
        d.process_frame(7, &[psg(0xFF)]);
        let ev = d.events();
        assert_eq!(ev.len(), 4);
        assert_eq!(
            ev[0].instrument, ev[1].instrument,
            "tono = un solo instrumento"
        );
        assert_ne!(ev[0].instrument, ev[2].instrument, "ruido ≠ tono");
        assert_ne!(
            ev[2].instrument, ev[3].instrument,
            "control de ruido distinto"
        );
    }

    // Frame DAC "con señal": muestras 0x2A variando (rango ≥ umbral de silencio).
    fn dac_loud(a: u8, b: u8) -> Vec<AudioWrite> {
        let mut w = Vec::new();
        w.extend(fm_write(0x2A, a));
        w.extend(fm_write(0x2A, b));
        w
    }

    #[test]
    fn dac_stream_segments_by_silence() {
        let mut d = AudioEventDetector::new();
        // Habilitar DAC + streamear muestras que VARÍAN (con señal) por 3 frames.
        let mut on = Vec::new();
        on.extend(fm_write(0x2B, 0x80)); // DAC enable
        on.extend(fm_write(0x2A, 0x40)); // rango 0x40..0xC0 → con señal
        on.extend(fm_write(0x2A, 0xC0));
        d.process_frame(100, &on);
        d.process_frame(101, &dac_loud(0x30, 0xD0));
        d.process_frame(102, &dac_loud(0x50, 0xB0)); // último frame con señal
        assert_eq!(d.event_count(), 0, "sigue sonando, sin evento cerrado");
        // Silencio (sin data) por DAC_GAP_FRAMES → cierra en el último frame con señal.
        for f in 103..=108 {
            d.process_frame(f, &[]);
        }
        assert_eq!(d.event_count(), 1);
        let e = d.events()[0];
        assert_eq!(e.chip, CHIP_FM);
        assert_eq!(e.channel, 5, "FM6 · DAC");
        assert_eq!(e.start_frame, 100);
        assert_eq!(e.end_frame, 102, "la cola de silencio NO entra al bloque");
        assert_ne!(e.signature, 0);
        assert_eq!(
            e.instrument, e.signature,
            "DAC: el hash del PCM ES el instrumento"
        );
    }

    #[test]
    fn dac_two_sfx_separated_by_silence_are_two_events() {
        let mut d = AudioEventDetector::new();
        d.process_frame(0, &{
            let mut w = Vec::new();
            w.extend(fm_write(0x2B, 0x80));
            w.extend(fm_write(0x2A, 0x20));
            w.extend(fm_write(0x2A, 0xE0));
            w
        });
        d.process_frame(1, &dac_loud(0x30, 0xC0)); // SFX 1: frames 0-1
        for f in 2..=8 {
            d.process_frame(f, &[]);
        } // silencio → cierra SFX 1
        d.process_frame(20, &dac_loud(0x10, 0xF0)); // SFX 2 arranca
        d.process_frame(21, &dac_loud(0x40, 0xB0));
        for f in 22..=28 {
            d.process_frame(f, &[]);
        } // silencio → cierra SFX 2
        assert_eq!(
            d.event_count(),
            2,
            "dos SFX separados por silencio → dos eventos"
        );
        assert_eq!(d.events()[0].start_frame, 0);
        assert_eq!(d.events()[1].start_frame, 20);
    }

    #[test]
    fn dac_suppresses_fm6_keyon() {
        // Con DAC habilitado, un key-on de FM6 (ch5) no debe abrir un evento FM
        // (el DAC posee el canal); el stream con señal sí.
        let mut d = AudioEventDetector::new();
        let mut w = Vec::new();
        w.extend(fm_write(0x2B, 0x80)); // DAC enable
        w.extend([fm(0x28, fm_keyon(5))]); // key-on FM6 (debe ignorarse)
        w.extend(fm_write(0x2A, 0x20));
        w.extend(fm_write(0x2A, 0xE0)); // stream con señal
        d.process_frame(0, &w);
        for f in 1..=7 {
            d.process_frame(f, &[]);
        } // silencio → cierra DAC
        assert_eq!(d.event_count(), 1, "sólo el evento DAC, no el FM key-on");
        assert_eq!(d.events()[0].channel, 5);
    }

    #[test]
    fn fm_invalid_channel_select_ignored() {
        let mut d = AudioEventDetector::new();
        // canal 3 (low 2 bits == 3) es inválido → no abre bloque
        d.process_frame(0, &[fm(0x28, 0xF0 | 0x03)]);
        d.finish();
        assert_eq!(d.event_count(), 0);
    }

    #[test]
    fn fm_port1_channels_3_to_5() {
        let mut d = AudioEventDetector::new();
        // key-on canal 4 (banco 1): low2=1, bit2 set
        d.process_frame(0, &[fm(0x28, fm_keyon(4))]);
        d.process_frame(5, &[fm(0x28, fm_keyoff(4))]);
        assert_eq!(d.event_count(), 1);
        assert_eq!(d.events()[0].channel, 4);
    }

    #[test]
    fn fm_unclosed_block_closed_by_finish() {
        let mut d = AudioEventDetector::new();
        d.process_frame(7, &[fm(0x28, fm_keyon(1))]);
        d.process_frame(40, &[]); // sigue sonando
        assert_eq!(d.event_count(), 0);
        d.finish();
        assert_eq!(d.event_count(), 1);
        let e = d.events()[0];
        assert_eq!(e.start_frame, 7);
        assert_eq!(e.end_frame, 40, "finish cierra en el último frame visto");
    }

    #[test]
    #[expect(
        clippy::identity_op,
        reason = "zero-valued PSG bit fields are kept explicit to document the register encoding"
    )]
    fn psg_attenuation_drives_key_on_off() {
        let mut d = AudioEventDetector::new();
        // canal 0 (tono): registro de atenuación = índice 1 → byte 0x90 | nib.
        // primero setear frecuencia (latch reg 0, LSB) + (data, MSB).
        d.process_frame(
            3,
            &[
                psg(0x80 | (0 << 4) | 0x0A), // latch reg0 (freq LSB ch0) nibble 0xA
                psg(0x1F),                   // data: MSB 6 bits
                psg(0x90 | 0x00),            // atenuación reg1 = 0 (volumen máx) → key-on
            ],
        );
        assert_eq!(d.event_count(), 0);
        d.process_frame(9, &[psg(0x90 | 0x0F)]); // atenuación 0xF = mudo → key-off
        assert_eq!(d.event_count(), 1);
        let e = d.events()[0];
        assert_eq!(e.chip, CHIP_PSG);
        assert_eq!(e.channel, 0);
        assert_eq!(e.start_frame, 3);
        assert_eq!(e.end_frame, 8); // mudo (key-off) en 9 → end 8
    }

    #[test]
    #[expect(
        clippy::identity_op,
        reason = "the zero attenuation field is explicit to document the PSG register encoding"
    )]
    fn psg_noise_channel_3() {
        let mut d = AudioEventDetector::new();
        // atenuación del canal de ruido = índice 7 → byte 0xF0 | nib.
        d.process_frame(0, &[psg(0xE5), psg(0xF0 | 0x00)]); // 0xE5 = latch reg6 (noise ctrl), luego atten reg7=0
        d.process_frame(2, &[psg(0xF0 | 0x0F)]); // mudo → key-off
        assert_eq!(d.event_count(), 1);
        assert_eq!(d.events()[0].channel, 3);
    }

    #[test]
    fn psg_starts_muted_no_spurious_event() {
        let mut d = AudioEventDetector::new();
        // atenuación 0xF de entrada (mudo) no debe abrir ni cerrar nada.
        d.process_frame(0, &[psg(0x90 | 0x0F)]);
        d.finish();
        assert_eq!(d.event_count(), 0);
    }

    #[test]
    fn reset_clears_state() {
        let mut d = AudioEventDetector::new();
        d.process_frame(0, &[fm(0x28, fm_keyon(0))]);
        d.process_frame(1, &[fm(0x28, fm_keyoff(0))]);
        assert_eq!(d.event_count(), 1);
        d.reset();
        assert_eq!(d.event_count(), 0);
        assert!(!d.fm_on[0]);
    }

    #[test]
    fn active_channels_reflects_live_key_on_off() {
        let mut d = AudioEventDetector::new();
        // ch0 y ch2 encendidos → ambos activos con su firma; ch0 apagado → sólo ch2.
        d.process_frame(0, &[fm(0x28, fm_keyon(0))]);
        d.process_frame(1, &[fm(0x28, fm_keyon(2))]);
        let act = d.active_channels();
        assert_eq!(act.len(), 2);
        assert!(
            act.iter()
                .any(|a| a.chip == CHIP_FM && a.channel == 0 && a.signature != 0)
        );
        assert!(act.iter().any(|a| a.chip == CHIP_FM && a.channel == 2));
        d.process_frame(2, &[fm(0x28, fm_keyoff(0))]);
        let act2 = d.active_channels();
        assert_eq!(act2.len(), 1);
        assert_eq!(act2[0].channel, 2);
        // clear_events no toca el estado de canales (sigue activo ch2).
        d.clear_events();
        assert_eq!(d.active_channels().len(), 1);
        assert_eq!(d.event_count(), 0);
    }

    // Sub exacta legacy (los campos de regla en su default) para los tests.
    fn sub(
        signature: u64,
        asset: &str,
        channels: u32,
        duration_frames: u32,
        looping: bool,
    ) -> EventSub {
        EventSub {
            signature,
            asset: asset.into(),
            channels,
            duration_frames,
            looping,
            match_rule: MATCH_EXACT,
            match_instrument: 0,
            match_pitch: 255,
            bus: 0,
        }
    }

    ///  el BUS va y vuelve. Sin esto el Lab lo escribe y el runtime lo
    /// tira, que es lo que hacía que en el juego los buses no separaran nada.
    #[test]
    fn el_bus_va_y_vuelve() {
        let mut a = sub(0xAA, "m.ogg", 0x3F, 0, true);
        a.bus = 1; // música
        let mut b = sub(0xBB, "s.ogg", 0x01, 0, false);
        b.bus = 2; // efectos
        let mut c = sub(0xCC, "v.ogg", 0x02, 0, false);
        c.bus = 3; // voces
        let text = events_to_toml(&[a, b, c]);
        let back = events_from_toml(&text);
        assert_eq!(back.len(), 3);
        assert_eq!((back[0].bus, back[1].bus, back[2].bus), (1, 2, 3));
    }

    /// AUSENTE = 0 = «sin clasificar», NO «Efectos».
    ///
    /// Un pack horneado antes de este campo no dijo nada, y tratarlo como
    /// Efectos haría que bajar ese bus le bajara el volumen a la música de
    /// todos los packs viejos. El default de «Efectos» vive en el Lab, para las
    /// asignaciones sueltas del autor; acá la ausencia es un dato.
    #[test]
    fn un_pack_viejo_no_queda_en_efectos() {
        let viejo = r#"
[[event]]
signature = "0x00000000000000aa"
asset = "m.ogg"
channels = "0x0000003f"
"#;
        let back = events_from_toml(viejo);
        assert_eq!(back.len(), 1);
        assert_eq!(back[0].bus, 0, "sin clasificar, no Efectos");

        // Y sin bus el TOML sale BYTE-IDÉNTICO al formato previo: un pack que
        // no clasifica nada no cambia de shape al re-hornearse.
        let sin = sub(0xAA, "m.ogg", 0x3F, 0, false);
        assert!(!events_to_toml(&[sin]).contains("bus"));
    }

    /// Un nombre desconocido cae a 0 y no a Efectos: un pack de mañana puede
    /// traer un bus nuevo, y meterlo en Efectos lo haría bajar con un slider
    /// que no es el suyo.
    #[test]
    fn un_bus_desconocido_no_cae_en_efectos() {
        let raro = r#"
[[event]]
signature = "0x00000000000000aa"
asset = "m.ogg"
channels = "0x0000003f"
bus = "ambiente"
"#;
        assert_eq!(events_from_toml(raro)[0].bus, 0);
    }

    #[test]
    fn event_sub_toml_round_trips() {
        let subs = vec![
            sub(
                0xDEAD_BEEF_CAFE_BABE,
                "audio/sfx/jump.ogg",
                0x0008,
                0,
                false,
            ),
            sub(
                0x0000_0000_0000_0001,
                "audio/music/lead.flac",
                0x0240,
                0,
                false,
            ),
            // SECUENCIA (Mezclar): ventana relativa al trigger + loop.
            sub(
                0x0000_0000_0000_00AB,
                "audio/music/zone1.ogg",
                0x03C7,
                1800,
                true,
            ),
        ];
        let toml = events_to_toml(&subs);
        let back = events_from_toml(&toml);
        assert_eq!(back, subs, "round-trip exacto del catálogo de eventos");
        // Un catálogo SIN secuencias ni reglas no emite los campos nuevos
        // (byte-compat con el formato previo).
        let plain = events_to_toml(&subs[..2]);
        assert!(
            !plain.contains("duration")
                && !plain.contains("loop")
                && !plain.contains("match")
                && !plain.contains("instrument"),
            "sin secuencias ni reglas, el TOML no cambia de shape"
        );
    }

    #[test]
    fn event_sub_toml_round_trips_match_rules() {
        // F3: la regla viaja con su identidad y vuelve idéntica.
        let mut a = sub(0x0000_0000_0000_0010, "audio/sfx/hit.wav", 0x0004, 0, false);
        a.match_rule = MATCH_INSTRUMENT;
        a.match_instrument = 0x2f35_4fbd_d69a_202d;
        let mut b = sub(
            0x0000_0000_0000_0020,
            "audio/sfx/note.wav",
            0x0002,
            0,
            false,
        );
        b.match_rule = MATCH_INSTRUMENT_PITCH;
        b.match_instrument = 0x878b_adc3_0542_24bb;
        b.match_pitch = 69;
        let subs = vec![a, b];
        let toml = events_to_toml(&subs);
        assert!(toml.contains("match = \"instrument\""));
        assert!(toml.contains("match = \"instrument_pitch\""));
        assert!(toml.contains("pitch = 69"));
        assert_eq!(events_from_toml(&toml), subs, "round-trip de las reglas");
        // Regla SIN instrumento no puede armarse → parsea como exacta.
        let broken = "[[event]]\nsignature = \"0x10\"\nasset = \"a.wav\"\n\
                      match = \"instrument\"\n\n";
        let back = events_from_toml(broken);
        assert_eq!(
            back[0].match_rule, MATCH_EXACT,
            "match sin instrument cae a exacta"
        );
        // Valor de match desconocido = tolerante (exacta), no error.
        let unknown = "[[event]]\nsignature = \"0x10\"\nasset = \"a.wav\"\n\
                       match = \"telepatia\"\ninstrument = \"0x99\"\n\n";
        assert_eq!(events_from_toml(unknown)[0].match_rule, MATCH_EXACT);
    }

    #[test]
    fn active_channels_carry_instrument_and_pitch() {
        // F3: el MISMO patch en otro canal, otra nota y otro pan es el
        // MISMO instrumento (firma distinta) — el fixture de la regla de match.
        let mut d = AudioEventDetector::new();
        let mut w = Vec::new();
        // Patch distintivo en ch0 (idx 0) y ch1 (idx 1): DT/MUL del op0.
        w.extend(fm_write(0x30, 0x71));
        w.extend(fm_write(0x31, 0x71));
        // ch0: A4 (fnum 1083 · block 4) pan centro; ch1: otra nota, pan izq.
        w.extend(fm_write(0xA4, 0x24));
        w.extend(fm_write(0xA0, 0x3B));
        w.extend(fm_write(0xB4, 0xC0));
        w.extend(fm_write(0xA5, 0x24));
        w.extend(fm_write(0xA1, 0x00));
        w.extend(fm_write(0xB5, 0x80));
        w.extend([fm(0x28, fm_keyon(0)), fm(0x28, fm_keyon(1))]);
        d.process_frame(1, &w);
        let act = d.active_channels();
        assert_eq!(act.len(), 2);
        let a0 = act.iter().find(|a| a.channel == 0).unwrap();
        let a1 = act.iter().find(|a| a.channel == 1).unwrap();
        assert_ne!(a0.instrument, 0, "el active trae el timbre al key-on");
        assert_eq!(
            a0.instrument, a1.instrument,
            "canal/nota/pan no fragmentan el instrumento"
        );
        assert_ne!(a0.signature, a1.signature, "la firma exacta sí distingue");
        assert_eq!(a0.pitch, 69, "A4 en ch0");
        assert_ne!(a1.pitch, a0.pitch, "la nota viaja en el active");
    }

    #[test]
    fn event_sub_toml_tolerates_missing_channels_and_bad_entries() {
        // channels omitido → 0; entrada sin asset se ignora.
        let text = "\
[[event]]\nsignature = \"0x00000000000000ff\"\nasset = \"a.ogg\"\n\n\
[[event]]\nsignature = \"0x0000000000000002\"\n\n"; // sin asset → ignorada
        let back = events_from_toml(text);
        assert_eq!(back.len(), 1);
        assert_eq!(back[0].signature, 0xff);
        assert_eq!(back[0].asset, "a.ogg");
        assert_eq!(back[0].channels, 0);
    }

    #[test]
    fn event_sub_empty_round_trips() {
        assert!(events_from_toml(&events_to_toml(&[])).is_empty());
    }

    #[test]
    fn interleaved_fm_channels_independent() {
        let mut d = AudioEventDetector::new();
        // ch0 y ch2 suenan solapados; deben dar dos eventos independientes.
        d.process_frame(0, &[fm(0x28, fm_keyon(0))]);
        d.process_frame(2, &[fm(0x28, fm_keyon(2))]);
        d.process_frame(5, &[fm(0x28, fm_keyoff(0))]);
        d.process_frame(8, &[fm(0x28, fm_keyoff(2))]);
        assert_eq!(d.event_count(), 2);
        let ch0 = d.events().iter().find(|e| e.channel == 0).unwrap();
        let ch2 = d.events().iter().find(|e| e.channel == 2).unwrap();
        assert_eq!((ch0.start_frame, ch0.end_frame), (0, 4)); // key-off en 5 → end 4
        assert_eq!((ch2.start_frame, ch2.end_frame), (2, 7)); // key-off en 8 → end 7
    }
}

// ============================================================================
// BatchEventDetector — detector por HASHES DE BATCH de PCM (C-A1, rama
// refinement). Complementa al detector por comandos de chip de arriba: corre
// sobre el historial de audio de una toma (.arp v7, un hash por frame) sin
// necesitar el log de escrituras del fork. Es el que consume
// AytherSession::resolve_audio_events via ayther_audio_evdet_* (FFI).
// Limite conocido: no separa sonidos DISTINTOS solapados en la mezcla (para
// eso esta el detector por canal de arriba).
// ============================================================================

use xxhash_rust::xxh3::xxh3_64;

/// Batches silenciosos consecutivos que cierran un run en vuelo (~0.1 s @60fps).
const SILENCE_GAP: u32 = 6;
/// Cuántos hashes de apertura alimentan la firma estable del run.
const SIGNATURE_HEAD: usize = 4;

/// One detected audio event: a contiguous run of non-silent batches.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatchAudioEvent {
    /// Per-occurrence instance id (monotonic) — distinguishes repeat plays.
    pub id: u64,
    /// Stable content key (xxhash3 of the first `SIGNATURE_HEAD` batch hashes).
    /// Matches across plays → the substitution/`audio_events.toml` key.
    pub signature: u64,
    /// Frame of the first non-silent batch (the "attack").
    pub start_frame: u64,
    /// Frame of the last non-silent batch (the "end").
    pub end_frame: u64,
    /// Number of non-silent batches in the run.
    pub batch_count: u32,
}

/// In-flight run state (before it closes into an `BatchAudioEvent`).
struct RunState {
    id: u64,
    start_frame: u64,
    head: Vec<u64>, // first SIGNATURE_HEAD non-silent hashes
    batch_count: u32,
    last_sound: u64, // frame of the most recent non-silent batch
    silence: u32,    // consecutive silent batches so far
}

/// Streaming detector: feed batch hashes, collect completed events.
pub struct BatchEventDetector {
    frame: u64,
    next_id: u64,
    run: Option<RunState>,
    completed: Vec<BatchAudioEvent>,
    /// Split a run when the sound's opening batch recurs (a **re-attack**), so a
    /// sound retriggered with no intervening silence becomes two events instead
    /// of one merged run (componentes-plan §2.6: "cierra tras N de silencio o
    /// cambio brusco de firma"). On by default.
    split_on_reattack: bool,
}

impl BatchEventDetector {
    /// Creates an empty detector with re-attack splitting enabled.
    pub fn new() -> Self {
        Self {
            frame: 0,
            next_id: 0,
            run: None,
            completed: Vec::new(),
            split_on_reattack: true,
        }
    }

    /// Toggle re-attack splitting (default on). Off = only silence closes a run
    /// (the pre-refinement behaviour).
    pub fn set_split_on_reattack(&mut self, on: bool) {
        self.split_on_reattack = on;
    }

    /// Feed one batch hash (`0` = silent, as `AudioHasher::process_batch` reports).
    /// Call once per audio batch (≈ once per game frame).
    pub fn push(&mut self, hash: u64) {
        let frame = self.frame;

        if hash != 0 {
            // Re-attack: the run's opening batch (its deterministic head) recurs
            // after the head is complete → the sound retriggered with no silence
            // between plays. Close the in-flight instance so the recurrence opens
            // a fresh one (the None arm below), giving distinct ids with the same
            // signature — e.g. rapid ring pickups back to back.
            let reattack = self.split_on_reattack
                && self.run.as_ref().is_some_and(|r| {
                    r.batch_count >= SIGNATURE_HEAD as u32 && r.head.first() == Some(&hash)
                });
            if reattack {
                self.close_run();
            }

            match &mut self.run {
                Some(r) => {
                    r.batch_count += 1;
                    if r.head.len() < SIGNATURE_HEAD {
                        r.head.push(hash);
                    }
                    r.last_sound = frame;
                    r.silence = 0;
                }
                None => {
                    let id = self.next_id;
                    self.next_id += 1;
                    self.run = Some(RunState {
                        id,
                        start_frame: frame,
                        head: vec![hash],
                        batch_count: 1,
                        last_sound: frame,
                        silence: 0,
                    });
                }
            }
        } else {
            // Silent batch: extend the gap; close the run once it is long enough.
            let mut close = false;
            if let Some(r) = &mut self.run {
                r.silence += 1;
                close = r.silence >= SILENCE_GAP;
            }
            if close {
                self.close_run();
            }
        }

        self.frame += 1;
    }

    /// Close any in-flight run (end of stream / recording).
    pub fn flush(&mut self) {
        if self.run.is_some() {
            self.close_run();
        }
    }

    fn close_run(&mut self) {
        if let Some(r) = self.run.take() {
            self.completed.push(BatchAudioEvent {
                id: r.id,
                signature: signature_of(&r.head),
                start_frame: r.start_frame,
                end_frame: r.last_sound,
                batch_count: r.batch_count,
            });
        }
    }

    /// Events completed so far (an in-flight run appears only after it closes or
    /// after `flush`).
    pub fn events(&self) -> &[BatchAudioEvent] {
        &self.completed
    }
    /// Returns the number of completed batch events.
    pub fn event_count(&self) -> usize {
        self.completed.len()
    }
}

impl Default for BatchEventDetector {
    fn default() -> Self {
        Self::new()
    }
}

/// Stable signature of a run from its opening batch hashes.
fn signature_of(head: &[u64]) -> u64 {
    let bytes: Vec<u8> = head.iter().flat_map(|h| h.to_le_bytes()).collect();
    xxh3_64(&bytes)
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod batch_tests {
    use super::*;

    #[test]
    fn detects_one_event_between_silence() {
        let mut d = BatchEventDetector::new();
        d.push(0);
        d.push(0); // frames 0,1 — silence
        d.push(0xA);
        d.push(0xB);
        d.push(0xC); // frames 2,3,4 — the sound
        for _ in 0..SILENCE_GAP {
            d.push(0);
        } // frames 5..=10 — trailing silence closes it

        assert_eq!(d.event_count(), 1);
        let e = &d.events()[0];
        assert_eq!(e.id, 0);
        assert_eq!(e.start_frame, 2);
        assert_eq!(e.end_frame, 4);
        assert_eq!(e.batch_count, 3);
        assert_ne!(e.signature, 0);
    }

    #[test]
    fn brief_gap_does_not_split_a_run() {
        let mut d = BatchEventDetector::new();
        d.push(0xA);
        d.push(0);
        d.push(0); // 2 silent (< SILENCE_GAP) — a gap inside the sound
        d.push(0xB);
        d.flush();

        assert_eq!(d.event_count(), 1);
        let e = &d.events()[0];
        assert_eq!(
            e.batch_count, 2,
            "the two sound batches, silence not counted"
        );
        assert_eq!(e.start_frame, 0);
        assert_eq!(e.end_frame, 3, "last sound batch (B) was at frame 3");
    }

    #[test]
    fn repeat_plays_share_signature_but_get_distinct_ids() {
        let mut d = BatchEventDetector::new();
        // First play.
        d.push(0xA);
        d.push(0xB);
        d.push(0xC);
        for _ in 0..SILENCE_GAP {
            d.push(0);
        }
        // Second play of the same sound (same opening hashes).
        d.push(0xA);
        d.push(0xB);
        d.push(0xC);
        d.flush();

        assert_eq!(d.event_count(), 2);
        let (a, b) = (&d.events()[0], &d.events()[1]);
        assert_eq!(a.signature, b.signature, "same head → same content key");
        assert_ne!(a.id, b.id, "distinct instances");
        assert_eq!((a.id, b.id), (0, 1));
    }

    #[test]
    fn different_sounds_have_different_signatures() {
        let mut d = BatchEventDetector::new();
        d.push(0xA);
        d.push(0xB);
        for _ in 0..SILENCE_GAP {
            d.push(0);
        }
        d.push(0xC);
        d.push(0xD);
        d.flush();

        assert_eq!(d.event_count(), 2);
        assert_ne!(d.events()[0].signature, d.events()[1].signature);
    }

    #[test]
    fn silence_only_produces_no_events() {
        let mut d = BatchEventDetector::new();
        for _ in 0..20 {
            d.push(0);
        }
        d.flush();
        assert_eq!(d.event_count(), 0);
    }

    #[test]
    fn reattack_splits_a_retrigger_without_silence() {
        // A sound plays (head A,B,C,D + tail E), then retriggers immediately
        // (head recurs, no silence). Should be two instances, same signature.
        let mut d = BatchEventDetector::new();
        d.push(0xA);
        d.push(0xB);
        d.push(0xC);
        d.push(0xD);
        d.push(0xE); // instance 1
        d.push(0xA);
        d.push(0xB);
        d.push(0xC);
        d.push(0xD); // re-attack
        d.flush();

        assert_eq!(d.event_count(), 2, "the re-attack splits the run");
        let (a, b) = (&d.events()[0], &d.events()[1]);
        assert_eq!((a.start_frame, a.end_frame), (0, 4));
        assert_eq!(b.start_frame, 5, "second instance starts at the re-attack");
        assert_eq!(a.signature, b.signature, "same head → same content key");
        assert_ne!(a.id, b.id);
        assert_eq!((a.id, b.id), (0, 1));
    }

    #[test]
    fn reattack_split_can_be_disabled() {
        let mut d = BatchEventDetector::new();
        d.set_split_on_reattack(false);
        d.push(0xA);
        d.push(0xB);
        d.push(0xC);
        d.push(0xD);
        d.push(0xE);
        d.push(0xA);
        d.push(0xB);
        d.push(0xC);
        d.push(0xD);
        d.flush();
        assert_eq!(
            d.event_count(),
            1,
            "off → only silence splits, one merged run"
        );
        assert_eq!(d.events()[0].batch_count, 9);
    }

    #[test]
    fn reattack_within_the_head_does_not_split() {
        // A repeated opening hash *inside* the head window must not split (the
        // guard requires the head to be complete first).
        let mut d = BatchEventDetector::new();
        d.push(0xA);
        d.push(0xA);
        d.push(0xB);
        d.push(0xC);
        d.flush();
        assert_eq!(d.event_count(), 1);
        assert_eq!(d.events()[0].batch_count, 4);
    }

    #[test]
    fn signature_uses_only_the_head_so_a_varying_tail_still_matches() {
        let mut d = BatchEventDetector::new();
        // Two plays share the first SIGNATURE_HEAD hashes but differ afterwards.
        let head = [0x1, 0x2, 0x3, 0x4];
        for &h in &head {
            d.push(h);
        }
        d.push(0xAAAA); // divergent tail
        for _ in 0..SILENCE_GAP {
            d.push(0);
        }
        for &h in &head {
            d.push(h);
        }
        d.push(0xBBBB); // different tail
        d.flush();

        assert_eq!(d.event_count(), 2);
        assert_eq!(d.events()[0].signature, d.events()[1].signature);
    }
}

#[cfg(test)]
mod velocity_e_instrumento_tests {
    use super::*;

    fn fm(addr: u16, data: u8) -> AudioWrite {
        // `cycle` no participa de nada acá: no es replay-estable y el detector
        // trabaja por FRAME, que es la resolución nativa de esta música.
        AudioWrite {
            cycle: 0,
            addr,
            data,
            chip: CHIP_FM,
        }
    }

    /// Escribe el patch del canal 0: algoritmo `alg`, y `tl` en el registro de
    /// Total Level del operador `op`.
    fn set_tl(w: &mut Vec<AudioWrite>, op: usize, tl: u8) {
        w.extend([fm(0x40 + op as u16 * 4, tl)]);
    }
    fn set_alg(w: &mut Vec<AudioWrite>, alg: u8) {
        w.extend([fm(0xB0, alg)]);
    }
    fn key_on(w: &mut Vec<AudioWrite>) {
        w.extend([fm(0x28, 0xF0)]);
    }
    fn key_off(w: &mut Vec<AudioWrite>) {
        w.extend([fm(0x28, 0x00)]);
    }

    /// El corazón de  bajar el VOLUMEN no puede cambiar la identidad del
    /// timbre. Medido sobre Sonic, incluir el TL del portador daba 43
    /// identidades donde hay 30 — y en un canal, dieciséis donde hay una.
    #[test]
    fn el_volumen_no_cambia_el_instrumento() {
        // Algoritmo 0: sólo el operador 3 (bit 3) es portador.
        let inst_de = |tl_portador: u8| -> u64 {
            let mut d = AudioEventDetector::new();
            let mut w = Vec::new();
            set_alg(&mut w, 0);
            set_tl(&mut w, 0, 20); // modulador: timbre
            set_tl(&mut w, 3, tl_portador); // portador: volumen
            key_on(&mut w);
            d.process_frame(0, &w);
            let mut off = Vec::new();
            key_off(&mut off);
            d.process_frame(1, &off);
            d.finish();
            d.events()[0].instrument
        };
        assert_eq!(
            inst_de(0),
            inst_de(64),
            "el TL del PORTADOR es volumen: no debe cambiar el instrumento"
        );
    }

    /// Y el complemento: el TL de un MODULADOR sí es timbre (es el índice de
    /// modulación), así que ahí la identidad TIENE que cambiar. Sin este caso,
    /// el test de arriba pasaría también con una firma que ignore todos los TL.
    #[test]
    fn el_timbre_del_modulador_si_cambia_el_instrumento() {
        let inst_de = |tl_modulador: u8| -> u64 {
            let mut d = AudioEventDetector::new();
            let mut w = Vec::new();
            set_alg(&mut w, 0);
            set_tl(&mut w, 0, tl_modulador);
            set_tl(&mut w, 3, 10);
            key_on(&mut w);
            d.process_frame(0, &w);
            let mut off = Vec::new();
            key_off(&mut off);
            d.process_frame(1, &off);
            d.finish();
            d.events()[0].instrument
        };
        assert_ne!(
            inst_de(0),
            inst_de(40),
            "el TL de un MODULADOR es timbre: tiene que cambiar el instrumento"
        );
    }

    /// El volumen no se pierde: se va a `velocity`, que es donde corresponde.
    #[test]
    fn el_volumen_sale_por_velocity() {
        let vel_de = |tl_portador: u8| -> u8 {
            let mut d = AudioEventDetector::new();
            let mut w = Vec::new();
            set_alg(&mut w, 0);
            set_tl(&mut w, 3, tl_portador);
            key_on(&mut w);
            d.process_frame(0, &w);
            let mut off = Vec::new();
            key_off(&mut off);
            d.process_frame(1, &off);
            d.finish();
            d.events()[0].velocity
        };
        let fuerte = vel_de(0); // TL 0 = máximo
        let flojo = vel_de(96);
        assert!(
            fuerte > flojo,
            "menos TL = más fuerte: {fuerte} tendría que ser > {flojo}"
        );
        assert_eq!(fuerte, 127, "TL 0 es el máximo de la escala MIDI");
    }

    // -----------------------------------------------------------------------
    // PCM de Sega CD
    //
    // Este chip no llega por escrituras crudas sino por eventos ya tipificados,
    // así que los tests los construyen a mano — sin ROM, sin BIOS y sin
    // emulador, igual que los de FM y PSG.
    // -----------------------------------------------------------------------

    fn pcm_on(ch: u8, st: u8, ls: u16, fd: u16, env: u8) -> PcmEvent {
        PcmEvent {
            kind: PCM_KEY_ON,
            channel: ch,
            env,
            pan: 0xFF,
            st,
            _pad: 0,
            ls,
            fd,
        }
    }
    fn pcm_off(ch: u8) -> PcmEvent {
        PcmEvent {
            kind: PCM_KEY_OFF,
            channel: ch,
            ..Default::default()
        }
    }

    #[test]
    fn pcm_abre_y_cierra_un_bloque_por_canal() {
        let mut d = AudioEventDetector::new();
        d.process_frame_ex(10, &[], &[pcm_on(3, 0x2A, 0xBEEF, 0x0800, 0xC0)]);
        d.process_frame_ex(11, &[], &[]);
        d.process_frame_ex(12, &[], &[pcm_off(3)]);
        d.finish();
        let ev = d.events();
        assert_eq!(ev.len(), 1, "un key-on + su key-off = un evento");
        assert_eq!(ev[0].chip, CHIP_PCM);
        assert_eq!(ev[0].channel, 3);
        assert_eq!(
            (ev[0].start_frame, ev[0].end_frame),
            (10, 11),
            "el bloque abarca [key-on, key-off-1], igual que FM y PSG"
        );
        assert_eq!(
            ev[0].pitch, 60,
            "fd = 0x800 es la velocidad original del sample = la nota de anclaje"
        );
    }

    /// El chip no tiene tono: transporta el sample leyéndolo más rápido o más
    /// lento, igual que un sampler. La razón contra 1:1 ES el intervalo.
    #[test]
    fn la_velocidad_del_pcm_se_lee_como_nota() {
        assert_eq!(
            pcm_pitch(0x0800),
            60,
            "velocidad original = nota de anclaje"
        );
        assert_eq!(
            pcm_pitch(0x1000),
            72,
            "al doble de velocidad, una octava arriba"
        );
        assert_eq!(pcm_pitch(0x0400), 48, "a la mitad, una octava abajo");
        assert_eq!(pcm_pitch(0x2000), 84, "dos octavas arriba");
        assert_eq!(
            pcm_pitch(0),
            NO_PITCH,
            "canal parado: no hay nota que mostrar"
        );
        // Un semitono son 2^(1/12) ≈ 1.0595 → 0x800 * 1.0595 ≈ 0x87D
        assert_eq!(pcm_pitch(0x087D), 61, "un semitono arriba");
        // Fuera de la escala MIDI se declara sin altura en vez de saturar en un
        // número que mentiría sobre el intervalo.
        assert_eq!(pcm_pitch(1), NO_PITCH, "tan lento que se sale de la escala");
    }

    /// Lo que el pitch habilita: la regla de match instrumento+nota puede
    /// separar dos disparos del MISMO sample a velocidades distintas.
    #[test]
    fn el_mismo_sample_a_otra_velocidad_es_otra_nota() {
        let de = |fd: u16| -> (u64, u64, u8) {
            let mut d = AudioEventDetector::new();
            d.process_frame_ex(0, &[], &[pcm_on(0, 0x2A, 0x1000, fd, 0xC0)]);
            d.process_frame_ex(1, &[], &[pcm_off(0)]);
            d.finish();
            let e = d.events()[0];
            (e.instrument, e.signature, e.pitch)
        };
        let (inst_a, sig_a, pitch_a) = de(0x0800);
        let (inst_b, sig_b, pitch_b) = de(0x1000);
        assert_eq!(inst_a, inst_b, "mismo sample = mismo instrumento");
        assert_ne!(sig_a, sig_b, "…pero la velocidad sí entra en la firma");
        assert_ne!(
            pitch_a, pitch_b,
            "…y también en la nota, que es lo que las separa"
        );
        assert_eq!((pitch_a, pitch_b), (60, 72));
    }

    /// EL MOTIVO DEL CAMBIO EN EL FORK. Mismo volumen, misma velocidad, sample
    /// distinto: si la identidad no mirara st/ls, estos dos serían el mismo
    /// sonido y una sustitución asignada a uno dispararía con el otro.
    #[test]
    fn dos_samples_distintos_al_mismo_volumen_y_velocidad_no_se_confunden() {
        let firma_de = |st: u8, ls: u16| -> (u64, u64) {
            let mut d = AudioEventDetector::new();
            d.process_frame_ex(0, &[], &[pcm_on(0, st, ls, 0x0800, 0xC0)]);
            d.process_frame_ex(1, &[], &[pcm_off(0)]);
            d.finish();
            (d.events()[0].signature, d.events()[0].instrument)
        };
        let (sig_a, inst_a) = firma_de(0x2A, 0x1000);
        let (sig_b, inst_b) = firma_de(0x71, 0x1000);
        assert_ne!(sig_a, sig_b, "otro sample = otra firma");
        assert_ne!(inst_a, inst_b, "otro sample = otro instrumento");
        // y el loop también distingue: dos samples pueden arrancar igual
        let (_, inst_c) = firma_de(0x2A, 0x2000);
        assert_ne!(inst_a, inst_c, "el loop también forma parte del sample");
    }

    /// El mismo sample a otra velocidad es OTRA nota del MISMO instrumento —
    /// exactamente el criterio de FM, donde la frecuencia entra en la firma
    /// pero no en el instrumento.
    #[test]
    fn el_mismo_sample_a_otra_velocidad_comparte_instrumento() {
        let de = |fd: u16, env: u8, ch: u8| -> (u64, u64, u8) {
            let mut d = AudioEventDetector::new();
            d.process_frame_ex(0, &[], &[pcm_on(ch, 0x2A, 0x1000, fd, env)]);
            d.process_frame_ex(1, &[], &[pcm_off(ch)]);
            d.finish();
            let e = d.events()[0];
            (e.signature, e.instrument, e.velocity)
        };
        let (sig_grave, inst_grave, _) = de(0x0400, 0xC0, 0);
        let (sig_agudo, inst_agudo, _) = de(0x0C00, 0xC0, 0);
        assert_ne!(sig_grave, sig_agudo, "otra velocidad = otra firma");
        assert_eq!(
            inst_grave, inst_agudo,
            "…pero el mismo sample = el mismo instrumento"
        );

        // el volumen tampoco es identidad: se va a velocity
        let (_, inst_fuerte, vel_fuerte) = de(0x0400, 0xFF, 0);
        let (_, inst_flojo, vel_flojo) = de(0x0400, 0x20, 0);
        assert_eq!(
            inst_fuerte, inst_flojo,
            "el volumen no es identidad de timbre"
        );
        assert!(
            vel_fuerte > vel_flojo,
            "…pero sale por velocity: {vel_fuerte} > {vel_flojo}"
        );

        // el canal no fragmenta el instrumento: el driver los rota
        let (_, inst_ch0, _) = de(0x0400, 0xC0, 0);
        let (_, inst_ch7, _) = de(0x0400, 0xC0, 7);
        assert_eq!(
            inst_ch0, inst_ch7,
            "el mismo sample desde otro canal es el mismo instrumento"
        );
    }

    /// El driver reapunta un canal a otro sample SIN apagarlo. Sin cerrar el
    /// bloque en el re-disparo, dos sonidos quedarían dentro de uno solo.
    #[test]
    fn el_redisparo_sin_key_off_cierra_el_bloque_anterior() {
        let mut d = AudioEventDetector::new();
        d.process_frame_ex(5, &[], &[pcm_on(1, 0x2A, 0x1000, 0x0800, 0xC0)]);
        d.process_frame_ex(9, &[], &[pcm_on(1, 0x71, 0x1000, 0x0800, 0xC0)]);
        d.process_frame_ex(12, &[], &[pcm_off(1)]);
        d.finish();
        let ev = d.events();
        assert_eq!(ev.len(), 2, "dos disparos = dos bloques");
        assert_eq!((ev[0].start_frame, ev[0].end_frame), (5, 8));
        assert_eq!((ev[1].start_frame, ev[1].end_frame), (9, 11));
        assert_ne!(ev[0].instrument, ev[1].instrument);
    }

    #[test]
    fn el_pcm_vivo_aparece_en_active_channels() {
        let mut d = AudioEventDetector::new();
        d.process_frame_ex(0, &[], &[pcm_on(6, 0x2A, 0x1000, 0x0800, 0xC0)]);
        let act = d.active_channels();
        let a = act
            .iter()
            .find(|a| a.chip == CHIP_PCM && a.channel == 6)
            .expect("el canal PCM encendido tiene que estar vivo para el runtime");
        assert_ne!(a.signature, 0);
        assert_ne!(a.instrument, 0);
        assert_eq!(a.pitch, 60, "la voz viva lleva su nota, como FM y PSG");

        d.process_frame_ex(1, &[], &[pcm_off(6)]);
        assert!(
            !d.active_channels().iter().any(|a| a.chip == CHIP_PCM),
            "tras el key-off ya no está vivo"
        );
    }

    /// Volumen y pitch a mitad de un sonido NO cambian de qué sonido se trata:
    /// la identidad es la del momento del disparo (mismo criterio que en FM,
    /// donde un fundido no fragmenta el instrumento).
    #[test]
    fn volumen_y_pitch_a_mitad_no_cambian_la_identidad() {
        let mut d = AudioEventDetector::new();
        d.process_frame_ex(0, &[], &[pcm_on(2, 0x2A, 0x1000, 0x0800, 0xC0)]);
        let sig_al_disparo = d.active_channels()[0].signature;
        d.process_frame_ex(
            1,
            &[],
            &[
                PcmEvent {
                    kind: PCM_VOLUME,
                    channel: 2,
                    env: 0x10,
                    ..Default::default()
                },
                PcmEvent {
                    kind: PCM_PITCH,
                    channel: 2,
                    fd: 0x0C00,
                    ..Default::default()
                },
            ],
        );
        assert_eq!(d.active_channels()[0].signature, sig_al_disparo);
        assert_eq!(
            d.events().len(),
            0,
            "un cambio de volumen no cierra ni abre bloques"
        );
    }

    /// El chip queda fuera del camino de FM/PSG y viceversa: process_frame sin
    /// eventos de PCM no puede inventar bloques de un chip que no habló.
    #[test]
    fn sin_eventos_de_pcm_no_hay_bloques_de_pcm() {
        let mut d = AudioEventDetector::new();
        let mut w = Vec::new();
        set_alg(&mut w, 0);
        key_on(&mut w);
        d.process_frame(0, &w);
        let mut off = Vec::new();
        key_off(&mut off);
        d.process_frame(1, &off);
        d.finish();
        assert!(
            !d.events().is_empty(),
            "NO-VACUIDAD: el FM sí produjo su bloque"
        );
        assert!(d.events().iter().all(|e| e.chip != CHIP_PCM));
        assert!(!d.active_channels().iter().any(|a| a.chip == CHIP_PCM));
    }

    #[test]
    fn finish_cierra_el_pcm_que_quedo_sonando() {
        let mut d = AudioEventDetector::new();
        d.process_frame_ex(7, &[], &[pcm_on(0, 0x2A, 0x1000, 0x0800, 0xC0)]);
        d.process_frame_ex(20, &[], &[]);
        d.finish();
        let ev = d.events();
        assert_eq!(ev.len(), 1);
        assert_eq!(
            (ev[0].start_frame, ev[0].end_frame),
            (7, 20),
            "sin key-off, el bloque llega hasta el último frame visto"
        );
    }
}

// ---------------------------------------------------------------------------
// — GATE de condiciones por firma
//
// Una sustitución de audio puede llevar condiciones (`[[event.condition]]`) con
// el MISMO dialecto que las de tiles: `memory_const`, `memory_check`,
// `frame_range`. El sonido HD sólo suena si TODAS dan verdadero.
//
// Por qué vive acá y no del lado C++: el evaluador es `crate::conditions` y ya
// lo consume `tile_substitutor`. Tener DOS evaluadores —uno por medio— es la
// clase de duplicación que este repo ya pagó con los tres consumidores del
// `addr` de audio que se desincronizaron dos días (ver audio_output_smoke).
//
// A diferencia de los tiles, acá NO hay orden de match que preservar: hay UNA
// sustitución por firma, así que la condición es un GATE booleano y no un
// desempate entre variantes.
// ---------------------------------------------------------------------------

use crate::conditions::{CondSpec, Condition, FrameCtx, build_conditions, eval_all};

/// Compiled event conditions indexed by trigger signature.
pub struct AudioEventGate {
    by_sig: Vec<(u64, Vec<Condition>)>,
}

impl AudioEventGate {
    /// Compiles conditional entries from `audio_events.toml`.
    ///
    /// Events without conditions are omitted because their implicit behavior is
    /// to remain enabled.
    pub fn from_toml(text: &str) -> Self {
        let mut by_sig = Vec::new();
        let tbl: toml::Value = match toml::from_str(text) {
            Ok(t) => t,
            Err(_) => return Self { by_sig },
        };
        let arr = match tbl.get("event").and_then(|v| v.as_array()) {
            Some(a) => a,
            None => return Self { by_sig },
        };
        for e in arr {
            let sig = match e
                .get("signature")
                .and_then(|v| v.as_str())
                .and_then(parse_hex_u64)
            {
                Some(s) => s,
                None => continue,
            };
            let raw = match e.get("condition").and_then(|v| v.as_array()) {
                Some(c) if !c.is_empty() => c,
                _ => continue,
            };
            let specs: Vec<CondSpec> = raw
                .iter()
                .filter_map(|v| v.clone().try_into::<CondSpec>().ok())
                .collect();
            if specs.len() != raw.len() {
                eprintln!("[AudioGate] 0x{sig:016x}: condición malformada — entrada ignorada");
                continue;
            }
            // Una condición que no compila DESCARTA la entrada, igual que en
            // los tiles: aplicarla igual la volvería incondicional, que es lo
            // contrario de lo que el autor pidió.
            match build_conditions(&specs) {
                Ok(c) if !c.is_empty() => by_sig.push((sig, c)),
                Ok(_) => {}
                Err(msg) => eprintln!("[AudioGate] 0x{sig:016x}: {msg} — entrada ignorada"),
            }
        }
        Self { by_sig }
    }

    /// Returns whether the catalog contains no conditional event entries.
    pub fn is_empty(&self) -> bool {
        self.by_sig.is_empty()
    }

    /// Returns signatures whose conditions do not match this frame.
    pub fn blocked(&self, ctx: &FrameCtx) -> Vec<u64> {
        self.by_sig
            .iter()
            .filter(|(_, c)| !eval_all(c, ctx))
            .map(|(s, _)| *s)
            .collect()
    }
}

#[cfg(test)]
mod gate_tests {
    use super::*;
    use crate::conditions::RamView;

    const DOC: &str = r#"
[[event]]
signature = "0x0000000000000001"
asset = "aa"
[[event.condition]]
kind = "memory_const"
addr = 16
value = 7
op = "eq"

[[event]]
signature = "0x0000000000000002"
asset = "bb"
"#;

    #[test]
    fn sin_condiciones_no_entra_al_gate() {
        let g = AudioEventGate::from_toml(DOC);
        let ram = vec![0u8; 64];
        let ctx = FrameCtx::new(0, RamView::linear(&ram));
        // 0x2 no tiene condiciones: nunca puede estar bloqueada.
        assert!(!g.blocked(&ctx).contains(&2));
    }

    #[test]
    fn la_condicion_decide() {
        let g = AudioEventGate::from_toml(DOC);
        let mut ram = vec![0u8; 64];

        ram[16] = 0;
        let ctx = FrameCtx::new(0, RamView::linear(&ram));
        assert!(g.blocked(&ctx).contains(&1), "no se cumple ⇒ bloqueada");

        ram[16] = 7;
        let ctx = FrameCtx::new(0, RamView::linear(&ram));
        assert!(!g.blocked(&ctx).contains(&1), "se cumple ⇒ suena el HD");
    }

    #[test]
    fn condicion_rota_descarta_la_entrada() {
        // `op` inválido: la entrada se ignora en vez de volverse incondicional.
        let doc = "[[event]]\nsignature = \"0x1\"\nasset = \"aa\"\n\
                   [[event.condition]]\nkind = \"memory_const\"\naddr = 1\n\
                   value = 1\nop = \"jamas\"\n";
        let g = AudioEventGate::from_toml(doc);
        assert!(g.is_empty());
    }

    #[test]
    fn documento_roto_no_crashea() {
        assert!(AudioEventGate::from_toml("[[event").is_empty());
    }
}
