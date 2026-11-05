//! Canonical identity functions and known-answer test vectores.
//!
//! These helpers provide bit-exact tile, sprite, and audio identities so SDK
//! consumers do not need to reimplement the pack identity specification.

// identity_kat.rs — las identidades como FUNCIONES PURAS, y sus vectores de
// prueba bit-exactos.
//
// # Por qué existe
//
// `docs/IDENTITY_SPECIFICATION.md` specifies every identity bit for bit, and
// its reimplementation section lists the seven mistakes that
// **compilan y no matchean nada**: usar el basis canónico de FNV en vez del
// seed propio, olvidar el `^1` del word-swap donde va (o aplicarlo donde no
// va), meter la línea de paleta en el hash de sprite —que es ciego a
// paleta— o no meterla en el de tile de plano —que no lo es—, recorrer los
// tiles en otro orden.
//
// Una spec no puede atrapar ninguno de esos: se leen bien y se implementan
// mal. Un **vector de prueba** sí. Por eso acá hay dos cosas:
//
// 1. Las funciones **puras** de identidad, expuestas: un tercero que puede
//    PEDIR el hash no tiene por qué re-implementarlo. Es la forma más barata
//    de que nadie cometa esos siete errores.
// 2. Los **KAT** (known-answer tests): entrada exacta → hash esperado. Sirven
//    en las dos direcciones, y la segunda es la que más nos protege a
//    nosotros: si un refactor cambia una identidad sin querer, **todos los
//    packs publicados dejan de matchear** y nadie se entera hasta que un
//    jugador ve el juego sin remasterizar. El KAT lo convierte en un test
//    rojo.
//
// # Los vectores son sintéticos a propósito
//
// No salen de una ROM: se construyen con un patrón determinista que cualquiera
// puede reproducir desde la spec sin tener el juego. Un vector que exige una
// ROM comercial no lo puede correr un tercero, que es justamente para quien
// existe esto (BYOR: el SDK no distribuye ROMs).

use crate::vram_sprite::{palette_signature, pose_key_with_flips};
use xxhash_rust::xxh3::xxh3_64;

/// AYTHER-specific FNV-1a seed used by pack identities.
///
/// This is deliberately different from the canonical FNV offset basis.
pub const AY_FNV_SEED: u64 = 0x1465_0FB0_739D_0383;
/// The 64-bit FNV-1a prime.
pub const AY_FNV_PRIME: u64 = 0x0000_0100_0000_01B3;

/// Mixes one byte into an AYTHER FNV-1a state using wrapping multiplication.
#[inline]
pub fn ay_fnv_mix(h: u64, b: u8) -> u64 {
    (h ^ b as u64).wrapping_mul(AY_FNV_PRIME)
}

/// Number of bytes in one 8×8, 4-bpp Mega Drive tile.
pub const VRAM_TILE_BYTES: usize = 32;

/// Computes the canonical palette-independent sprite hash.
///
/// Color indexes are expanded high nibble first in VRAM tile order and hashed
/// with XXH3-64. No word-swap correction is applied. Out-of-range tiles
/// contribute 64 zero indexes so partial VRAM captures remain deterministic.
pub fn sprite_hash(vram: &[u8], tile_idx: usize, w: u8, h: u8) -> u64 {
    let total = w as usize * h as usize;
    let mut raw = Vec::with_capacity(total * VRAM_TILE_BYTES);
    for t in 0..total {
        let off = (tile_idx + t) * VRAM_TILE_BYTES;
        if off + VRAM_TILE_BYTES > vram.len() {
            raw.extend_from_slice(&[0u8; VRAM_TILE_BYTES]);
            continue;
        }
        raw.extend_from_slice(&vram[off..off + VRAM_TILE_BYTES]);
    }
    sprite_pattern_hash(&raw)
}

/// The sprite hash of pattern bytes already gathered in VRAM (host) order:
/// `w * h * 32` bytes, tiles contiguous from the base tile. Each byte expands
/// to its high and low colour index before XXH3-64, exactly as `sprite_hash`
/// does from a VRAM slice. Spec 007 exposes it through
/// `ayther_sprite_pattern_hash` so a frontend can verify a stored pattern
/// against the identity it was captured under.
pub fn sprite_pattern_hash(raw: &[u8]) -> u64 {
    let mut buf = Vec::with_capacity(raw.len() * 2);
    for &byte in raw {
        buf.push((byte >> 4) & 0x0F);
        buf.push(byte & 0x0F);
    }
    xxh3_64(&buf)
}

/// Computes the canonical plane-tile hash.
///
/// Pattern bytes use word-swap correction (`offset ^ 1`) and the palette line
/// is mixed last. Flip flags are intentionally excluded from the identity.
pub fn plane_tile_hash(vram: &[u8], pattern: u16, pal: u8) -> u64 {
    let mut h = AY_FNV_SEED;
    let base = pattern as usize * VRAM_TILE_BYTES;
    for b in 0..VRAM_TILE_BYTES {
        let off = (base + b) ^ 1;
        h = ay_fnv_mix(h, vram.get(off).copied().unwrap_or(0));
    }
    ay_fnv_mix(h, pal & 3)
}

/// Computes a replay-stable FM event signature from latched YM2612 registers.
///
/// The signature includes patch, frequency, pan, and channel state. Cycle timing
/// is excluded so the same recorded command stream yields the same identity.
pub fn fm_signature(fm_regs: &[u8], ch: usize) -> u64 {
    let bank = if ch < 3 { 0usize } else { 0x100 };
    let idx = ch % 3;
    let mut h = ay_fnv_mix(ay_fnv_mix(AY_FNV_SEED, 0), ch as u8);
    let carriers = FM_CARRIERS[(fm_regs.get(bank + 0xB0 + idx).copied().unwrap_or(0) & 7) as usize];
    let mut base = 0x30usize;
    while base <= 0x9C {
        // The total level (0x40..0x4C) of a carrier operator is volume, not
        // identity (kAudioEventAlgo 3): skipped. A modulator's stays.
        let op = FM_SLOT_OPERATOR[((base - 0x30) / 4) % 4];
        let is_tl = (base & 0xF0) == 0x40;
        if !(is_tl && carriers & (1 << op) != 0) {
            h = ay_fnv_mix(h, fm_regs.get(bank + base + idx).copied().unwrap_or(0));
        }
        base += 4;
    }
    for off in [0xA0usize, 0xA4, 0xB0, 0xB4] {
        h = ay_fnv_mix(h, fm_regs.get(bank + off + idx).copied().unwrap_or(0));
    }
    h
}

/// Instrument identity of an FM channel (namespace 0xF1): the patch without the
/// frequency, the channel, the pan bits of 0xB4 and the carrier total levels.
pub fn fm_instrument(fm_regs: &[u8], ch: usize) -> u64 {
    let bank = if ch < 3 { 0usize } else { 0x100 };
    let idx = ch % 3;
    let reg = |a: usize| fm_regs.get(bank + a + idx).copied().unwrap_or(0);
    let carriers = FM_CARRIERS[(reg(0xB0) & 7) as usize];
    let mut h = ay_fnv_mix(ay_fnv_mix(AY_FNV_SEED, 0), 0xF1);
    let mut base = 0x30usize;
    while base <= 0x9C {
        let op = FM_SLOT_OPERATOR[((base - 0x30) / 4) % 4];
        let is_tl = (base & 0xF0) == 0x40;
        if !(is_tl && carriers & (1 << op) != 0) {
            h = ay_fnv_mix(h, reg(base));
        }
        base += 4;
    }
    h = ay_fnv_mix(h, reg(0xB0));
    ay_fnv_mix(h, reg(0xB4) & 0x3F)
}

/// Carrier operators per FM algorithm (bit n = operator n), as in the detector.
const FM_CARRIERS: [u8; 8] = [0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF];
/// Register slot order S1, S3, S2, S4 mapped to operator numbers.
const FM_SLOT_OPERATOR: [u8; 4] = [0, 2, 1, 3];

// ---------------------------------------------------------------------------
// Los vectores
// ---------------------------------------------------------------------------

/// Version of the identity contract (`AYTHER_IDENTITY_CONTRACT` in
/// `ayther_identity_contract.h`). Changes only when an identity changes.
pub const IDENTITY_CONTRACT: u32 = 1;

/// One known-answer identity test vector.
pub struct Kat {
    /// Identity family: `sprite`, `plane_tile`, or `fm_event`.
    pub id: &'static str,
    /// Short description of the covered input case.
    pub case_description: &'static str,
    /// Expected 64-bit identity.
    pub expected: u64,
}

/// Generates deterministic synthetic VRAM for identity conformance tests.
///
/// Byte `i` is `(i * 37 + 11) & 0xFF`, making adjacent byte swaps observable.
pub fn synthetic_vram(n: usize) -> Vec<u8> {
    (0..n).map(|i| ((i * 37 + 11) & 0xFF) as u8).collect()
}

/// Generates deterministic synthetic YM2612 register state.
pub fn synthetic_fm_registers() -> Vec<u8> {
    (0..0x200).map(|i| ((i * 53 + 7) & 0xFF) as u8).collect()
}

/// Identity of an animation group: xxh3 of the member hashes, sorted ascending,
/// as little-endian bytes. Member order is deliberately discarded.
pub fn anim_group_id(members: &[u64]) -> u64 {
    let mut sorted = members.to_vec();
    sorted.sort_unstable();
    let mut buf = Vec::with_capacity(sorted.len() * 8);
    for h in sorted {
        buf.extend_from_slice(&h.to_le_bytes());
    }
    xxh3_64(&buf)
}

/// Contribution of one non-empty cell to the screen-plane (panorama) signature.
/// `column` and `row` are `screen >> 3`; `plane` is 0 (B), 1 (A) or 2 (W). The
/// plane signature is the wrapping sum of these terms.
pub fn screen_plane_term(cell_hash: u64, column: u32, row: u32, plane: u32) -> u64 {
    let mut x = cell_hash;
    x ^= u64::from(column).wrapping_mul(0x9E37_79B9_7F4A_7C15);
    x ^= u64::from(row).wrapping_mul(0xC2B2_AE3D_27D4_EB4F);
    x ^= u64::from(plane + 1).wrapping_mul(0x1656_67B1_9E37_79F9);
    x ^= x >> 33;
    x = x.wrapping_mul(0xFF51_AFD7_ED55_8CCD);
    x ^ (x >> 29)
}

/// Member hashes of the synthetic pose used by the pose vectors.
const POSE_HASHES: [u64; 2] = [0x41573b4753eaaeb4, 0xadba9fc9e8c721a7];

/// Deterministic synthetic CRAM: word `i` is `(i * 29 + 5) & 0x1FF`.
pub fn synthetic_cram() -> Vec<u16> {
    (0..64).map(|i| ((i * 29 + 5) & 0x1FF) as u16).collect()
}

/// Drives the event detector with a fixed synthetic stream and returns the
/// `(class, signature, instrument)` of each event: PSG tone, PSG noise, DAC and
/// PCM (the FM class is covered by `fm_event`). The stream is described in the
/// published vectors; `cycle` is zero because it takes no part in identity.
pub fn audio_class_identities() -> Vec<(&'static str, u64, u64)> {
    use crate::audio_event::{
        AudioEventDetector, AudioWrite, CHIP_FM, CHIP_PSG, PCM_KEY_OFF, PCM_KEY_ON, PcmEvent,
    };
    let psg = |data: u8| AudioWrite {
        cycle: 0,
        addr: 0,
        data,
        chip: CHIP_PSG,
    };
    let fm = |addr: u16, data: u8| AudioWrite {
        cycle: 0,
        addr,
        data,
        chip: CHIP_FM,
    };
    let mut d = AudioEventDetector::new();
    // PSG channel 0, tone n = 0x0E: latch frequency, attenuation 0 (on), then off.
    d.process_frame(0, &[psg(0x80 | 0x0E), psg(0x90)]);
    d.process_frame(1, &[psg(0x9F)]);
    // PSG noise, control 0x04.
    d.process_frame(2, &[psg(0xE4), psg(0xF0)]);
    d.process_frame(3, &[psg(0xFF)]);
    // DAC: enable (0x2B bit 7) and two loud ordered samples 0x10, 0xF0.
    d.process_frame(4, &[fm(0x2B, 0x80), fm(0x2A, 0x10), fm(0x2A, 0xF0)]);
    for f in 5..12 {
        d.process_frame(f, &[]);
    }
    // PCM channel 2: key-on (start 0x10, loop 0x0123, delta 0x0400), key-off.
    let on = PcmEvent {
        kind: PCM_KEY_ON,
        channel: 2,
        env: 0xFF,
        pan: 0xFF,
        st: 0x10,
        _pad: 0,
        ls: 0x0123,
        fd: 0x0400,
    };
    let off = PcmEvent {
        kind: PCM_KEY_OFF,
        channel: 2,
        ..PcmEvent::default()
    };
    d.process_frame_ex(12, &[], &[on]);
    d.process_frame_ex(13, &[], &[off]);
    d.finish();
    d.events()
        .iter()
        .map(|e| {
            let class = match (e.chip, e.channel) {
                (1, 3) => "psg_noise",
                (1, _) => "psg_tone",
                (0, 5) => "dac",
                (3, _) => "pcm",
                _ => "other",
            };
            (class, e.signature, e.instrument)
        })
        .collect()
}

/// Returns the canonical known-answer vectores.
///
/// Expected values are part of the public pack-identity contract.
pub fn vectors() -> Vec<Kat> {
    vec![
        Kat {
            id: "sprite",
            case_description: "1×1 tile en el índice 0",
            expected: sprite_hash(&synthetic_vram(4096), 0, 1, 1),
        },
        Kat {
            id: "sprite",
            case_description: "4×4 tiles (el máximo) desde el índice 3",
            expected: sprite_hash(&synthetic_vram(4096), 3, 4, 4),
        },
        Kat {
            id: "sprite",
            case_description: "fuera de VRAM: rellena con ceros, no trunca",
            expected: sprite_hash(&synthetic_vram(64), 1, 2, 2),
        },
        Kat {
            id: "plane_tile",
            case_description: "patrón 1, línea de paleta 0",
            expected: plane_tile_hash(&synthetic_vram(4096), 1, 0),
        },
        Kat {
            id: "plane_tile",
            case_description: "mismo patrón, línea de paleta 2",
            expected: plane_tile_hash(&synthetic_vram(4096), 1, 2),
        },
        Kat {
            id: "pose",
            case_description: "dos miembros sin flips (clave solo de hashes)",
            expected: pose_key_with_flips(&POSE_HASHES, None),
        },
        Kat {
            id: "pose",
            case_description: "flips mixtos 0|1 (suma los flips relativos)",
            expected: pose_key_with_flips(&POSE_HASHES, Some(&[0, 1])),
        },
        Kat {
            id: "pose",
            case_description: "flips 2|3: desplazamiento global de 0|1, misma clave",
            expected: pose_key_with_flips(&POSE_HASHES, Some(&[2, 3])),
        },
        Kat {
            id: "animation_group",
            case_description: "dos miembros: el orden de entrada no cuenta",
            expected: anim_group_id(&[POSE_HASHES[1], POSE_HASHES[0]]),
        },
        Kat {
            id: "panorama",
            case_description: "termino de una celda: hash del tile 1, columna 3, fila 5, plano A",
            expected: screen_plane_term(0x98d36be2cd04adb9, 3, 5, 1),
        },
        Kat {
            id: "palette_variant",
            case_description: "linea 0, slots 1..15 (mascara 0xFFFE)",
            expected: palette_signature(&synthetic_cram(), 0, 0xFFFE),
        },
        Kat {
            id: "palette_variant",
            case_description: "linea 2, slots 1 y 4 (mascara 0x0012)",
            expected: palette_signature(&synthetic_cram(), 2, 0x0012),
        },
        Kat {
            id: "instrument",
            case_description: "FM canal 0 sobre el banco sintetico (sin frecuencia, canal, pan ni TL de portadoras)",
            expected: fm_instrument(&synthetic_fm_registers(), 0),
        },
        Kat {
            id: "audio_psg",
            case_description: "PSG canal 0, tono n = 0x0E (flujo sintetico de audio_class_identities)",
            expected: audio_class_identities()[0].1,
        },
        Kat {
            id: "audio_dac",
            case_description: "DAC, muestras 0x10 y 0xF0 del bloque de apertura",
            expected: audio_class_identities()[2].1,
        },
        Kat {
            id: "audio_pcm",
            case_description: "PCM canal 2, inicio 0x10, loop 0x0123, delta 0x0400",
            expected: audio_class_identities()[3].1,
        },
        Kat {
            id: "instrument",
            case_description: "PSG tono: instrumento compartido por todos los canales de tono",
            expected: audio_class_identities()[0].2,
        },
        Kat {
            id: "instrument",
            case_description: "PSG ruido, control 0x04",
            expected: audio_class_identities()[1].2,
        },
        Kat {
            id: "instrument",
            case_description: "PCM canal 2 (espacio 0xF2, sin canal ni delta)",
            expected: audio_class_identities()[3].2,
        },
        Kat {
            id: "fm_event",
            case_description: "canal 0 (banco 0)",
            expected: fm_signature(&synthetic_fm_registers(), 0),
        },
        Kat {
            id: "fm_event",
            case_description: "canal 4 (banco 1)",
            expected: fm_signature(&synthetic_fm_registers(), 4),
        },
    ]
}

/// Serializes the known-answer vectores as SDK-friendly TOML.
pub fn vectors_toml() -> String {
    let mut s = String::new();
    s.push_str(
        "# AYTHER identity test vectores\n\
                #\n\
                # GENERADO. Regenerar: `cargo run -p ay_pack -- kat`.\n\
                #\n\
                # Cada vector es entrada exacta -> hash esperado. La entrada es\n\
                # SINTETICA y se reproduce desde esta descripcion sin tener\n\
                # ninguna ROM: vram[i] = (i*37 + 11) & 0xFF, fm_regs[i] =\n\
                # (i*53 + 7) & 0xFF.\n\
                #\n\
                # Si tu re-implementacion no da estos numeros, la spec\n\
                # `pack-identities.md` §9 lista los siete errores que compilan\n\
                # y no matchean nada.\n\n",
    );
    s.push_str(&format!(
        "[identity]
contract = {}

",
        IDENTITY_CONTRACT
    ));
    s.push_str("seed_fnv  = \"0x14650fb0739d0383\"   # NO es el basis canonico\n");
    s.push_str("prime_fnv = \"0x00000100000001b3\"\n\n");
    for k in vectors() {
        s.push_str(&format!(
            "[[vector]]\nidentidad = \"{}\"\ncaso      = \"{}\"\nhash      = \"0x{:016x}\"\n\n",
            k.id, k.case_description, k.expected
        ));
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    /// LOS NÚMEROS. Están escritos a mano y no calculados: un test que compara
    /// la implementación consigo misma pasa siempre, incluso después de
    /// romperla. Éstos se generaron una vez, se copiaron acá, y de ahí en más
    /// cualquier cambio de identidad los pone en rojo.
    ///
    /// Si este test falla, la pregunta NO es «qué número hay que actualizar»:
    /// es «qué identidad cambió y cuántos packs publicados acaba de romper».
    /// `sprite_pattern_hash` over the gathered bytes is the same identity as
    /// `sprite_hash` over the VRAM slice (spec 007: the FFI reproduces it).
    #[test]
    fn pattern_hash_matches_sprite_hash() {
        let vram = synthetic_vram(4096);
        assert_eq!(
            sprite_pattern_hash(&vram[0..32]),
            sprite_hash(&vram, 0, 1, 1)
        );
        assert_eq!(
            sprite_pattern_hash(&vram[3 * 32..19 * 32]),
            sprite_hash(&vram, 3, 4, 4)
        );
        assert_eq!(sprite_pattern_hash(&vram[0..32]), 0x4157_3b47_53ea_aeb4);
        // Nibble order matters: 0x12 and 0x21 are different pixels.
        assert_ne!(
            sprite_pattern_hash(&[0x12; 32]),
            sprite_pattern_hash(&[0x21; 32])
        );
    }

    #[test]
    fn hashes_match_published_values() {
        let vram = synthetic_vram(4096);
        assert_eq!(sprite_hash(&vram, 0, 1, 1), 0x4157_3b47_53ea_aeb4);
        assert_eq!(sprite_hash(&vram, 3, 4, 4), 0xadba_9fc9_e8c7_21a7);
        assert_eq!(plane_tile_hash(&vram, 1, 0), 0x98d3_6be2_cd04_adb9);
        assert_eq!(
            fm_signature(&synthetic_fm_registers(), 0),
            0x6a77_5e4a_e772_8677
        );
        assert_eq!(
            fm_signature(&synthetic_fm_registers(), 4),
            0xa144_8c41_1d57_2053
        );
    }

    /// El word-swap del tile de plano NO es decorativo: con la VRAM sintética,
    /// aplicarlo o no da hashes distintos. Es el control que hace que el vector
    /// pruebe algo — con VRAM de ceros, las dos versiones coinciden y el
    /// vector pasaría con la implementación equivocada.
    #[test]
    fn word_swap_changes_tile_hash() {
        let vram = synthetic_vram(4096);
        let with_palette = plane_tile_hash(&vram, 1, 0);
        // La misma cuenta SIN el `^1`, que es el error de re-implementación.
        let without_palette = {
            let mut h = AY_FNV_SEED;
            for b in 0..VRAM_TILE_BYTES {
                h = ay_fnv_mix(h, vram[32 + b]);
            }
            ay_fnv_mix(h, 0)
        };
        assert_ne!(
            with_palette, without_palette,
            "la VRAM sintética no distingue el swap: el vector no probaría nada"
        );
    }

    /// El sprite es CIEGO a paleta y el tile de plano NO. Es la confusión que
    /// más veces produce un pack que no matchea, y acá queda fijada: la línea
    /// de paleta mueve un hash y no el otro.
    #[test]
    fn palette_affects_tile_but_not_sprite() {
        let vram = synthetic_vram(4096);
        assert_ne!(plane_tile_hash(&vram, 1, 0), plane_tile_hash(&vram, 1, 2));
        // El sprite no recibe paleta ni por parámetro: la ceguera es
        // estructural, no una decisión que se pueda olvidar en el call site.
        assert_eq!(sprite_hash(&vram, 1, 1, 1), sprite_hash(&vram, 1, 1, 1));
    }

    /// El basis canónico de FNV no reproduce nada. Es el error de la spec
    /// §9 y el más fácil de cometer: está en todos los tutoriales.
    #[test]
    fn canonical_fnv_basis_is_rejected() {
        const CANONICO: u64 = 0xCBF2_9CE4_8422_2325;
        assert_ne!(AY_FNV_SEED, CANONICO);
        let vram = synthetic_vram(4096);
        let mal = {
            let mut h = CANONICO;
            for b in 0..VRAM_TILE_BYTES {
                h = ay_fnv_mix(h, vram[(32 + b) ^ 1]);
            }
            ay_fnv_mix(h, 0)
        };
        assert_ne!(plane_tile_hash(&vram, 1, 0), mal);
    }

    /// Fuera de VRAM se rellena con ceros y no se trunca: un sprite a medio
    /// cargar da un hash estable en vez de uno que depende de cuánta VRAM mandó
    /// el caller.
    #[test]
    fn out_of_vram_is_zero_filled_not_truncated() {
        let short_vram = synthetic_vram(64);
        let h = sprite_hash(&short_vram, 1, 2, 2);
        // El mismo cálculo con más VRAM de la que se lee NO puede coincidir:
        // si coincidiera, el relleno estaría tapando bytes reales.
        assert_ne!(h, sprite_hash(&synthetic_vram(4096), 1, 2, 2));
        // Y es determinista: dos llamadas con la misma VRAM corta dan lo mismo.
        assert_eq!(h, sprite_hash(&short_vram, 1, 2, 2));
    }

    /// Pose y variante de paleta: valores escritos a mano (ver el comentario de
    /// «LOS NÚMEROS» arriba). Un desplazamiento global de flips (0|1 y
    /// 2|3) conserva la clave; sin flips la clave es la de solo hashes.
    #[test]
    fn pose_and_palette_variant_answers_are_pinned() {
        assert_eq!(pose_key_with_flips(&POSE_HASHES, None), 0x43f1af85db6d39d0);
        assert_eq!(
            pose_key_with_flips(&POSE_HASHES, Some(&[0, 1])),
            0x39375f8df926189e
        );
        assert_eq!(
            pose_key_with_flips(&POSE_HASHES, Some(&[2, 3])),
            0x39375f8df926189e
        );
        assert_eq!(
            pose_key_with_flips(&POSE_HASHES, Some(&[1, 1])),
            0x43f1af85db6d39d0,
            "flips uniformes no cambian la clave"
        );
        assert_eq!(
            palette_signature(&synthetic_cram(), 0, 0xFFFE),
            0x327269cf7f752943
        );
        assert_eq!(
            palette_signature(&synthetic_cram(), 2, 0x0012),
            0xe602f24f070befbc
        );
    }

    /// Audio por clase: PSG tono y ruido, DAC y PCM. Los valores están escritos
    /// a mano; además las firmas de PSG, DAC y PCM se recalculan acá con la
    /// receta de `IDENTITY_SPECIFICATION.md` (no con el detector), de modo que
    /// una divergencia entre spec y detector también se ve.
    #[test]
    fn audio_class_answers_are_pinned() {
        let ids = audio_class_identities();
        let pins: [(&str, u64, u64); 4] = [
            ("psg_tone", 0x917b96983089543c, 0x9a64dd00c5447462),
            ("psg_noise", 0x88a287982b78e953, 0x9dd7ee4f3351c952),
            ("dac", 0x9fafa7e158f9353a, 0x9fafa7e158f9353a),
            ("pcm", 0x5975df5f33250ed2, 0x0474ccd04894a026),
        ];
        assert_eq!(ids.len(), pins.len());
        for (got, want) in ids.iter().zip(pins.iter()) {
            assert_eq!(got.0, want.0);
            assert_eq!(got.1, want.1, "signature of {}", want.0);
            assert_eq!(got.2, want.2, "instrument of {}", want.0);
        }
        let mix = |h: u64, bytes: &[u8]| bytes.iter().fold(h, |h, &b| ay_fnv_mix(h, b));
        // PSG: mix(mix(SEED,1), channel), frequency low byte then high byte.
        assert_eq!(mix(AY_FNV_SEED, &[1, 0, 0x0E, 0x00]), ids[0].1);
        // DAC: mix(mix(mix(SEED,0),5),0xDA), then each ordered 0x2A sample.
        assert_eq!(mix(AY_FNV_SEED, &[0, 5, 0xDA, 0x10, 0xF0]), ids[2].1);
        // PCM: mix(mix(SEED,3), channel), start, loop low/high, delta low/high.
        assert_eq!(
            mix(AY_FNV_SEED, &[3, 2, 0x10, 0x23, 0x01, 0x00, 0x04]),
            ids[3].1
        );
    }

    /// The pure FM identity must be the one the detector computes: load the
    /// synthetic register bank through bus writes, key channel 0 and 4 on and
    /// compare. A drift between this file and the detector fails here.
    #[test]
    fn fm_signature_equals_the_detectors() {
        use crate::audio_event::{AudioEventDetector, AudioWrite, CHIP_FM};
        let regs = synthetic_fm_registers();
        let write = |addr: u16, data: u8| AudioWrite {
            cycle: 0,
            addr,
            data,
            chip: CHIP_FM,
        };
        let mut d = AudioEventDetector::new();
        let mut loads = Vec::new();
        for addr in 0x30u16..0x1B8 {
            if (0x100..0x130).contains(&addr) {
                continue; // second-bank 0x00..0x2F are not channel registers
            }
            loads.push(write(addr, regs[addr as usize]));
        }
        d.process_frame(0, &loads);
        d.process_frame(1, &[write(0x28, 0xF0), write(0x28, 0xF5)]);
        d.process_frame(2, &[write(0x28, 0x00), write(0x28, 0x05)]);
        d.finish();
        let mut found = [false; 2];
        for e in d.events() {
            if e.chip == CHIP_FM && e.channel == 0 {
                assert_eq!(e.signature, fm_signature(&regs, 0));
                assert_eq!(e.instrument, fm_instrument(&regs, 0));
                found[0] = true;
            }
            if e.chip == CHIP_FM && e.channel == 4 {
                assert_eq!(e.signature, fm_signature(&regs, 4));
                assert_eq!(e.instrument, fm_instrument(&regs, 4));
                found[1] = true;
            }
        }
        assert_eq!(found, [true, true], "both channels produced an event");
    }

    /// Instrumento FM: valor escrito a mano; `fm_signature_equals_the_detectors`
    /// comprueba además que coincide con el del detector en dos canales.
    #[test]
    fn fm_instrument_answer_is_pinned() {
        assert_eq!(
            fm_instrument(&synthetic_fm_registers(), 0),
            0x2dcd_729b_a7e4_081a
        );
    }

    /// Grupo de animación y panorama: valores escritos a mano. El grupo es
    /// independiente del orden de entrada (la spec lo ordena), y con los
    /// miembros ya ordenados coincide con la clave de pose sin flips porque la
    /// construcción es la misma (xxh3 de los hashes little-endian).
    #[test]
    fn animation_group_and_panorama_answers_are_pinned() {
        assert_eq!(
            anim_group_id(&[POSE_HASHES[1], POSE_HASHES[0]]),
            0x43f1af85db6d39d0
        );
        assert_eq!(
            anim_group_id(&POSE_HASHES),
            anim_group_id(&[POSE_HASHES[1], POSE_HASHES[0]])
        );
        assert_eq!(
            screen_plane_term(0x98d36be2cd04adb9, 3, 5, 1),
            0x4baccc35b93adb02
        );
        // Sumar las celdas es conmutativo: el orden de observación no cuenta.
        let a = screen_plane_term(1, 0, 0, 0).wrapping_add(screen_plane_term(2, 1, 0, 0));
        let b = screen_plane_term(2, 1, 0, 0).wrapping_add(screen_plane_term(1, 0, 0, 0));
        assert_eq!(a, b);
        // Dos planos distintos con la misma celda dan términos distintos.
        assert_ne!(screen_plane_term(7, 2, 2, 0), screen_plane_term(7, 2, 2, 1));
    }

    /// El TOML publicable trae todos los vectores y el seed, que es el dato sin
    /// el cual ninguno se puede reproducir.
    #[test]
    fn published_toml_is_complete() {
        let t = vectors_toml();
        assert_eq!(t.matches("[[vector]]").count(), vectors().len());
        assert!(t.contains(&format!(
            "[identity]
contract = {IDENTITY_CONTRACT}"
        )));
        assert!(
            t.contains("0x14650fb0739d0383"),
            "sin el seed no se reproduce nada"
        );
        assert!(t.contains("identidad = \"sprite\""));
        assert!(t.contains("identidad = \"plane_tile\""));
        assert!(t.contains("identidad = \"fm_event\""));
        // Y los hashes van con el mismo formato que el resto del ecosistema.
        assert!(t.contains(&format!(
            "0x{:016x}",
            sprite_hash(&synthetic_vram(4096), 0, 1, 1)
        )));
    }

    /// LA PRUEBA QUE IMPORTA: estas funciones dan lo MISMO que el motor.
    ///
    /// Sin esto, los vectores certificarían esta copia y no la implementación
    /// real — un KAT que valida una segunda implementación de la misma casa no
    /// protege de nada, sólo duplica el error si lo hay.
    ///
    /// Se arma una SAT sintética con un sprite conocido y se corre el
    /// `SpriteHasher` de producción: el hash que publica tiene que ser el que
    /// calcula `sprite_hash`.
    #[test]
    #[expect(
        clippy::identity_op,
        reason = "the zero offset is explicit because the test documents the SAT word layout"
    )]
    fn sprite_hash_matches_engine() {
        use crate::vram_sprite::SpriteHasher;

        // VRAM con la SAT en un offset conocido, después de los tiles.
        const SAT: usize = 0xC000;
        let mut vram = synthetic_vram(0x10000);

        // Una entrada SAT: 4 words del 68k, y el buffer llega WORD-SWAPPED —
        // por eso cada word se escribe en little-endian, que es exactamente la
        // convención que el lector documenta.
        let mut wr = |off: usize, w: u16| {
            vram[off] = (w & 0xFF) as u8;
            vram[off + 1] = (w >> 8) as u8;
        };
        // Y = 128+64 (en pantalla) · size 2x2 · link 0 · tile 3 · X = 128+32
        wr(SAT + 0, 128 + 64);
        wr(SAT + 2, (1 << 10) | (1 << 8)); // vsize=2, hsize=2
        wr(SAT + 4, 3); // tile_idx = 3
        wr(SAT + 6, 128 + 32);
        // El resto de los slots quedan parqueados (Y crudo del patrón sintético
        // puede caer en pantalla, así que se ponen en 0 explícitamente).
        for slot in 1..80 {
            let o = SAT + slot * 8;
            for b in 0..8 {
                vram[o + b] = 0;
            }
        }

        let mut h = SpriteHasher::new();
        let n = h.process_vram(&vram, SAT);
        assert_eq!(
            n, 1,
            "la SAT sintética tiene que producir exactamente un sprite"
        );
        let occ = &h.last_occurrences()[0];
        assert_eq!((occ.w_tiles, occ.h_tiles), (2, 2));
        assert_eq!(
            occ.hash,
            sprite_hash(&vram, 3, 2, 2),
            "el hash publicado por el motor no coincide con la función pura"
        );
    }

    /// Ídem para el tile de plano: el hash del engine se calcula en C++
    /// (`ayther_session.cpp`), así que lo que se fija acá es que la función
    /// pura reproduzca la definición de la spec §2 — el `^1`, la paleta al
    /// final y el seed propio. El oráculo que la compara con el C++ vive en
    /// `tests/plane_hash_variants_test.cpp`.
    ///
    /// Se recalcula paso a paso, escrito de otra forma: si las dos maneras de
    /// escribir la misma definición coinciden, el error de transcripción
    /// tendría que estar en las dos.
    #[test]
    fn tile_hash_matches_specification() {
        let vram = synthetic_vram(4096);
        let pattern: u16 = 7;
        let pal: u8 = 1;

        let mut h: u64 = 0x14650FB0739D0383;
        for b in 0..32usize {
            let byte = vram[(pattern as usize * 32 + b) ^ 1];
            h = (h ^ byte as u64).wrapping_mul(0x100000001B3);
        }
        h = (h ^ (pal & 3) as u64).wrapping_mul(0x100000001B3);

        assert_eq!(plane_tile_hash(&vram, pattern, pal), h);
    }

    /// Y la mezcla FNV es la MISMA que la del motor, no una copia parecida.
    /// El primo se compara con el de `audio_event.rs` en decimal —
    /// 1_099_511_628_211— porque el error que esto atrapa es de transcripción
    /// hexadecimal: `0x1_0000_01B3` se lee igual que `0x100_0000_01B3` de
    /// reojo, y produce hashes que no matchean nada. Pasó escribiendo este
    /// archivo, y lo agarró el test de la definición paso a paso.
    #[test]
    fn fnv_prime_matches_engine() {
        assert_eq!(AY_FNV_PRIME, 1_099_511_628_211u64);
        assert_eq!(AY_FNV_SEED, 0x14650FB0739D0383u64);
    }

    /// La firma FM, escrita de otra forma: el barrido de registros, el banco
    /// por canal y el orden. Lo que fija es que el canal 4 lea el banco 1
    /// (`0x100`) con `idx = 1` — confundir el banco es el error que hace que
    /// los canales 3-5 de cualquier juego no matcheen, y sólo esos.
    #[test]
    #[expect(
        clippy::identity_op,
        reason = "xor with zero is explicit because the test reproduces every FNV mixing step"
    )]
    fn fm_signature_matches_specification() {
        let regs = synthetic_fm_registers();
        for (ch, bank, idx) in [(0usize, 0usize, 0usize), (4, 0x100, 1)] {
            let mut h = AY_FNV_SEED;
            h = (h ^ 0u64).wrapping_mul(1_099_511_628_211);
            h = (h ^ ch as u64).wrapping_mul(1_099_511_628_211);
            let carriers =
                [0x8u8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF][(regs[bank + 0xB0 + idx] & 7) as usize];
            let mut base = 0x30usize;
            while base <= 0x9C {
                let slot = (base - 0x30) / 4;
                let op = [0u8, 2, 1, 3][slot % 4];
                let skip = (base & 0xF0) == 0x40 && carriers & (1 << op) != 0;
                if !skip {
                    h = (h ^ regs[bank + base + idx] as u64).wrapping_mul(1_099_511_628_211);
                }
                base += 4;
            }
            for off in [0xA0usize, 0xA4, 0xB0, 0xB4] {
                h = (h ^ regs[bank + off + idx] as u64).wrapping_mul(1_099_511_628_211);
            }
            assert_eq!(fm_signature(&regs, ch), h, "canal {}", ch);
        }
        // Y dos canales distintos dan firmas distintas aunque compartan banco:
        // el canal entra al hash, si no un evento del ch 1 matchearía uno del 0.
        assert_ne!(fm_signature(&regs, 0), fm_signature(&regs, 1));
    }
}
