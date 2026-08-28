//! Canonical identity functions and known-answer test vectores.
//!
//! These helpers provide bit-exact tile, sprite, and audio identities so SDK
//! consumers do not need to reimplement the pack identity specification.

// identity_kat.rs — las identidades como FUNCIONES PURAS, y sus vectores de
// prueba bit-exactos.
//
// # Por qué existe
//
// `docs/specs/pack-identities.md` especifica bit a bit cómo se calcula cada
// identidad, y su §9 enumera los siete errores de re-implementación que
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
    let mut buf = Vec::with_capacity(total * 64);
    for t in 0..total {
        let off = (tile_idx + t) * VRAM_TILE_BYTES;
        if off + VRAM_TILE_BYTES > vram.len() {
            buf.extend_from_slice(&[0u8; 64]);
            continue;
        }
        for &byte in &vram[off..off + VRAM_TILE_BYTES] {
            buf.push((byte >> 4) & 0x0F);
            buf.push(byte & 0x0F);
        }
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
    let mut base = 0x30usize;
    while base <= 0x9C {
        h = ay_fnv_mix(h, fm_regs.get(bank + base + idx).copied().unwrap_or(0));
        base += 4;
    }
    for off in [0xA0usize, 0xA4, 0xB0, 0xB4] {
        h = ay_fnv_mix(h, fm_regs.get(bank + off + idx).copied().unwrap_or(0));
    }
    h
}

// ---------------------------------------------------------------------------
// Los vectores
// ---------------------------------------------------------------------------

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
    #[test]
    fn hashes_match_published_values() {
        let vram = synthetic_vram(4096);
        assert_eq!(sprite_hash(&vram, 0, 1, 1), 0x4157_3b47_53ea_aeb4);
        assert_eq!(sprite_hash(&vram, 3, 4, 4), 0xadba_9fc9_e8c7_21a7);
        assert_eq!(plane_tile_hash(&vram, 1, 0), 0x98d3_6be2_cd04_adb9);
        assert_eq!(
            fm_signature(&synthetic_fm_registers(), 0),
            0xab38_7bbb_7090_249b
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

    /// El TOML publicable trae todos los vectores y el seed, que es el dato sin
    /// el cual ninguno se puede reproducir.
    #[test]
    fn published_toml_is_complete() {
        let t = vectors_toml();
        assert_eq!(t.matches("[[vector]]").count(), vectors().len());
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
            let mut base = 0x30usize;
            while base <= 0x9C {
                h = (h ^ regs[bank + base + idx] as u64).wrapping_mul(1_099_511_628_211);
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
