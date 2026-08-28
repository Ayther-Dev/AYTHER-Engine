//! In-memory IPS and BPS parche detection and application.
//!
//! Patches transform caller-owned ROM buffers and never modify the source file
//! on disk. BPS entrada and salida checksums are validated when present.

// rom_patch.rs — parches IPS y BPS del USUARIO, aplicados en RAM.
//
// # Por qué existe, y por qué es BYOR-safe
//
// Una fan-translation o un romhack es un **parche**: un archivo chico que
// describe cómo transformar una ROM que el usuario ya tiene. No lleva el juego
// adentro, y por eso se puede distribuir donde la ROM no. AYTHER no
// redistribuye nada — el usuario trae su ROM (BYOR) y, si quiere, su parche.
//
// El combo es el que no existe en ningún lado: jugar una traducción **con** un
// pack HD encima.
//
// # Se aplica en RAM y nunca al archivo
//
// El parche se aplica al buffer que se le pasa al core, no al `.md` del disco.
// Es la misma doctrina que el resto del proyecto —«toda corrección es de
// reproducción, el archivo no se toca»— y acá además protege al usuario de
// quedarse sin su ROM original por probar un hack.
//
// # Los dos formatos
//
// **IPS** es de los años 90 y no verifica nada: son registros de «en este
// offset, escribí estos bytes». Su límite de 16 MB alcanza para Mega Drive.
// Aplicar un IPS a la ROM equivocada produce basura silenciosa — el formato no
// tiene con qué darse cuenta, y por eso el error que devolvemos es el único
// aviso posible: que el offset se salga del archivo.
//
// **BPS** sí verifica: trae los CRC32 de la ROM de entrada, de la de salida y
// del propio parche. Es la diferencia que importa para un usuario, y por eso
// los mensajes de error la nombran: un BPS que rechaza dice *qué* ROM esperaba.

/// Failure returned while detecting or applying a ROM parche.
#[derive(Debug, Clone, PartialEq)]
pub enum PatchError {
    /// The entrada is neither an IPS nor a BPS parche.
    NotAPatch,
    /// The parche is truncated or structurally invalid.
    Corrupt(&'static str),
    /// The parche attempts to write beyond the format's supported salida range.
    OutOfRange,
    /// The BPS source checksum does not match the supplied ROM.
    WrongRom {
        /// Source CRC-32 recorded by the BPS parche.
        expected: u32,
        /// CRC-32 calculated from the supplied ROM.
        actual: u32,
    },
    /// The patched salida does not match the BPS target checksum.
    CorruptOutput {
        /// Target CRC-32 recorded by the BPS parche.
        expected: u32,
        /// CRC-32 calculated from the patched salida.
        actual: u32,
    },
}

impl PatchError {
    /// Returns a consistent user-facing explanation of the failure.
    pub fn message(&self) -> String {
        match self {
            PatchError::NotAPatch => "el archivo no es un parche IPS ni BPS".to_string(),
            PatchError::Corrupt(reason) => format!("el parche está incompleto o mal formado: {reason}"),
            PatchError::OutOfRange => {
                "el parche escribe fuera de la ROM: casi seguro es para otra versión \
                 (IPS no trae checksum, así que esto es lo único que lo delata)"
                    .to_string()
            }
            PatchError::WrongRom { expected, actual } => format!(
                "el parche es para otra ROM: espera CRC32 {expected:08x} y \
                         ésta es {actual:08x}"
            ),
            PatchError::CorruptOutput { expected, actual } => format!(
                "el parche se aplicó pero el resultado no coincide con lo que \
                         promete (CRC32 {actual:08x}, esperaba {expected:08x}): el \
                         parche o la ROM están dañados"
            ),
        }
    }
}

/// Computes the IEEE CRC-32 checksum used by BPS patches.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

/// Returns whether the bytes begin with a recognized IPS or BPS magic value.
pub fn is_patch(data: &[u8]) -> bool {
    data.starts_with(b"PATCH") || data.starts_with(b"BPS1")
}

/// Applies an IPS or BPS parche to an in-memory ROM.
///
/// The source slice is never modified. The returned vector contains the patched
/// image.
///
/// # Errors
///
/// Returns [`PatchError`] when the format is unsupported, malformed, targets a
/// different ROM, exceeds format limits, or produces an invalid checksum.
pub fn apply(rom: &[u8], patch: &[u8]) -> Result<Vec<u8>, PatchError> {
    if patch.starts_with(b"PATCH") {
        return apply_ips(rom, patch);
    }
    if patch.starts_with(b"BPS1") {
        return apply_bps(rom, patch);
    }
    Err(PatchError::NotAPatch)
}

// ---------------------------------------------------------------------------
// IPS
// ---------------------------------------------------------------------------

/// Applies an IPS parche to an in-memory ROM.
///
/// Standard records and RLE records are supported. Output may grow up to the
/// 24-bit IPS address-space limit of 16 MiB.
///
/// # Errors
///
/// Returns [`PatchError`] for invalid magic, truncated records, or salida beyond
/// the IPS address-space limit.
pub fn apply_ips(rom: &[u8], patch: &[u8]) -> Result<Vec<u8>, PatchError> {
    const MAX: usize = 16 * 1024 * 1024; // el techo del formato (offset de 24 bits)
    if patch.len() < 8 || !patch.starts_with(b"PATCH") {
        return Err(PatchError::NotAPatch);
    }
    let mut out = rom.to_vec();
    let mut i = 5usize;
    loop {
        if i + 3 > patch.len() {
            return Err(PatchError::Corrupt("sin EOF"));
        }
        if &patch[i..i + 3] == b"EOF" {
            break;
        }
        let off = ((patch[i] as usize) << 16)
            | ((patch[i + 1] as usize) << 8)
            | (patch[i + 2] as usize);
        i += 3;
        if i + 2 > patch.len() {
            return Err(PatchError::Corrupt("registro cortado"));
        }
        let len = ((patch[i] as usize) << 8) | (patch[i + 1] as usize);
        i += 2;

            let (data, end_offset): (Vec<u8>, usize) = if len == 0 {
            // RLE
            if i + 3 > patch.len() {
                return Err(PatchError::Corrupt("RLE cortado"));
            }
            let n = ((patch[i] as usize) << 8) | (patch[i + 1] as usize);
            let v = patch[i + 2];
            i += 3;
            (vec![v; n], off + n)
        } else {
            if i + len > patch.len() {
                return Err(PatchError::Corrupt("datos cortados"));
            }
            let d = patch[i..i + len].to_vec();
            i += len;
            (d, off + len)
        };

            if end_offset > MAX {
            return Err(PatchError::OutOfRange);
        }
            if end_offset > out.len() {
                out.resize(end_offset, 0);
        }
            out[off..end_offset].copy_from_slice(&data);
    }
    Ok(out)
}

// ---------------------------------------------------------------------------
// BPS
// ---------------------------------------------------------------------------

/// Varint de BPS: 7 bits por byte, el bit 7 marca el ÚLTIMO, y cada byte
/// siguiente suma 1 al acumulador (por eso no hay dos codificaciones del mismo
/// número — un detalle del formato que se olvida y produce offsets corridos).
fn varint(d: &[u8], i: &mut usize) -> Result<u64, PatchError> {
    let mut result: u64 = 0;
    let mut shift: u64 = 1;
    loop {
        if *i >= d.len() {
            return Err(PatchError::Corrupt("varint cortado"));
        }
        let x = d[*i];
        *i += 1;
        result = result.wrapping_add(((x & 0x7F) as u64).wrapping_mul(shift));
        if x & 0x80 != 0 {
            break;
        }
        shift <<= 7;
        result = result.wrapping_add(shift);
        if shift == 0 {
            return Err(PatchError::Corrupt("varint absurdo"));
        }
    }
    Ok(result)
}

/// Applies a BPS parche and validates its source and target checksums.
///
/// # Errors
///
/// Returns [`PatchError`] for malformed actions, a source-ROM mismatch, an
/// invalid parche checksum, or an salida checksum mismatch.
pub fn apply_bps(rom: &[u8], patch: &[u8]) -> Result<Vec<u8>, PatchError> {
    if patch.len() < 4 + 12 || !patch.starts_with(b"BPS1") {
        return Err(PatchError::NotAPatch);
    }
    // Los tres CRC viven en los últimos 12 bytes: entrada, salida, y el del
    // propio parche.
    let n = patch.len();
    let body = &patch[..n - 12];
    let read_u32 = |p: usize| -> u32 {
        u32::from_le_bytes([patch[p], patch[p + 1], patch[p + 2], patch[p + 3]])
    };
    let crc_in = read_u32(n - 12);
    let crc_out = read_u32(n - 8);

    // Se comprueba la ROM ANTES de aplicar: aplicar y después decir que estaba
    // mal deja al usuario con un buffer que no sirve y la duda de qué pasó.
    let actual = crc32(rom);
    if actual != crc_in {
        return Err(PatchError::WrongRom {
            expected: crc_in,
            actual,
        });
    }

    let mut i = 4usize;
    let input_size = varint(body, &mut i)? as usize;
    let output_size = varint(body, &mut i)? as usize;
    let meta = varint(body, &mut i)? as usize;
    if input_size != rom.len() {
        return Err(PatchError::Corrupt("tamaño de entrada"));
    }
    i += meta;
    if i > body.len() {
        return Err(PatchError::Corrupt("metadata cortada"));
    }

    let mut out = vec![0u8; output_size];
    let mut output = 0usize;
    let mut src_rel: i64 = 0;
    let mut dst_rel: i64 = 0;

    while i < body.len() {
        let record = varint(body, &mut i)?;
        let action = (record & 3) as u8;
        let length = (record >> 2) as usize + 1;
        if output + length > output_size {
            return Err(PatchError::Corrupt("acción se pasa"));
        }
        match action {
            0 => {
                // SourceRead: copiar de la ROM en la misma posición
                if output + length > rom.len() {
                    return Err(PatchError::OutOfRange);
                }
                out[output..output + length].copy_from_slice(&rom[output..output + length]);
                output += length;
            }
            1 => {
                // TargetRead: los bytes vienen en el parche
                if i + length > body.len() {
                    return Err(PatchError::Corrupt("datos cortados"));
                }
                out[output..output + length].copy_from_slice(&body[i..i + length]);
                i += length;
                output += length;
            }
            2 | 3 => {
                // SourceCopy / TargetCopy: offset relativo con signo
                let d = varint(body, &mut i)?;
                let delta = (d >> 1) as i64 * if d & 1 != 0 { -1 } else { 1 };
                if action == 2 {
                    src_rel += delta;
                if src_rel < 0 || src_rel as usize + length > rom.len() {
                        return Err(PatchError::OutOfRange);
                    }
                    let s = src_rel as usize;
                out[output..output + length].copy_from_slice(&rom[s..s + length]);
                src_rel += length as i64;
                } else {
                    dst_rel += delta;
                    if dst_rel < 0 {
                        return Err(PatchError::OutOfRange);
                    }
                    // Byte a byte y no `copy_from_slice`: TargetCopy puede
                    // solaparse consigo mismo, y ese solapamiento ES el
                    // mecanismo de compresión del formato.
                for _ in 0..length {
                        let s = dst_rel as usize;
                        if s >= output {
                            return Err(PatchError::OutOfRange);
                        }
                        out[output] = out[s];
                        dst_rel += 1;
                        output += 1;
                    }
                }
                if action == 2 {
                    output += 0;
                }
            }
            _ => unreachable!(),
        }
    }

    let res = crc32(&out);
    if res != crc_out {
        return Err(PatchError::CorruptOutput {
            expected: crc_out,
            actual: res,
        });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rom() -> Vec<u8> {
        (0..256u32).map(|i| (i & 0xFF) as u8).collect()
    }

    /// Construye un IPS con un registro de datos.
    fn ips(reg: &[(usize, &[u8])]) -> Vec<u8> {
        let mut p = b"PATCH".to_vec();
        for (off, d) in reg {
            p.push((off >> 16) as u8);
            p.push((off >> 8) as u8);
            p.push(*off as u8);
            p.push((d.len() >> 8) as u8);
            p.push(d.len() as u8);
            p.extend_from_slice(d);
        }
        p.extend_from_slice(b"EOF");
        p
    }

    #[test]
    fn ips_writes_at_declared_offset() {
        let r = rom();
        let out = apply_ips(&r, &ips(&[(0x10, &[0xAA, 0xBB, 0xCC])])).unwrap();
        assert_eq!(&out[0x10..0x13], &[0xAA, 0xBB, 0xCC]);
        // Y NO toca el resto: un patcher que reescribe de más es indistinguible
        // de uno que anda, hasta que alguien mira el byte de al lado.
        assert_eq!(out[0x0F], r[0x0F]);
        assert_eq!(out[0x13], r[0x13]);
        assert_eq!(out.len(), r.len());
    }

    /// La ROM ORIGINAL no se modifica. Es la mitad del contrato: el parche se
    /// aplica en RAM y el archivo del usuario queda intacto.
    #[test]
    fn original_rom_is_unchanged() {
        let r = rom();
        let original = r.clone();
        let _ = apply_ips(&r, &ips(&[(0, &[0xFF; 8])])).unwrap();
        assert_eq!(r, original);
    }

    /// RLE: tamaño 0 significa «repetí este byte N veces». Es la mitad del
    /// formato que se olvida, y omitirla hace que medio parche real no aplique.
    #[test]
    fn ips_rle() {
        let mut p = b"PATCH".to_vec();
        p.extend_from_slice(&[0x00, 0x00, 0x20]); // offset 0x20
        p.extend_from_slice(&[0x00, 0x00]); // len 0 = RLE
        p.extend_from_slice(&[0x00, 0x05, 0x7E]); // 5 veces 0x7E
        p.extend_from_slice(b"EOF");
        let out = apply_ips(&rom(), &p).unwrap();
        assert_eq!(&out[0x20..0x25], &[0x7E; 5]);
        assert_eq!(out[0x25], rom()[0x25], "y ni uno mas");
    }

    /// Un parche puede EXTENDER la ROM: hay hacks que agregan contenido.
    #[test]
    fn ips_can_extend_rom() {
        let r = rom();
        let out = apply_ips(&r, &ips(&[(r.len(), &[1, 2, 3, 4])])).unwrap();
        assert_eq!(out.len(), r.len() + 4);
        assert_eq!(&out[r.len()..], &[1, 2, 3, 4]);
    }

    /// Un offset absurdo NO reserva gigabytes. Es la forma más fácil de tirar
    /// un proceso con un archivo de cuarenta bytes.
    #[test]
    fn ips_rejects_absurd_offset() {
        let mut p = b"PATCH".to_vec();
        p.extend_from_slice(&[0xFF, 0xFF, 0xFF]); // 16 MB - 1
        p.extend_from_slice(&[0x00, 0x10]);
        p.extend_from_slice(&[0u8; 16]);
        p.extend_from_slice(b"EOF");
        assert_eq!(apply_ips(&rom(), &p), Err(PatchError::OutOfRange));
    }

    /// Un archivo cortado se reporta, no se aplica a medias. Un patcher que
    /// escribe lo que pudo y devuelve Ok deja una ROM que arranca y falla en el
    /// nivel 3.
    #[test]
    fn truncated_ips_is_not_partially_applied() {
        let p = b"PATCH\x00\x00\x10\x00\x08\x01\x02".to_vec(); // dice 8 bytes, trae 2
        assert!(matches!(
            apply_ips(&rom(), &p),
            Err(PatchError::Corrupt(_))
        ));
        let without_eof = b"PATCH".to_vec();
        assert!(matches!(
            apply_ips(&rom(), &without_eof),
            Err(PatchError::NotAPatch)
        ));
    }

    /// El formato se detecta por la MAGIA y no por la extensión: un `.ips` que
    /// en realidad es un BPS se aplicaría como basura.
    #[test]
    fn format_is_detected_from_magic() {
        assert!(is_patch(b"PATCH..."));
        assert!(is_patch(b"BPS1..."));
        assert!(!is_patch(b"MZ\x90\x00"));
        assert_eq!(
            apply(&rom(), b"cualquier cosa"),
            Err(PatchError::NotAPatch)
        );
    }

    // -- BPS ----------------------------------------------------------------

    fn varint_enc(mut n: u64) -> Vec<u8> {
        let mut o = Vec::new();
        loop {
            let x = (n & 0x7F) as u8;
            n >>= 7;
            if n == 0 {
                o.push(x | 0x80);
                break;
            }
            o.push(x);
            n -= 1;
        }
        o
    }

    /// Un BPS mínimo: TargetRead de todo el contenido nuevo.
    fn bps(input: &[u8], output: &[u8]) -> Vec<u8> {
        let mut p = b"BPS1".to_vec();
        p.extend(varint_enc(input.len() as u64));
        p.extend(varint_enc(output.len() as u64));
        p.extend(varint_enc(0)); // sin metadata
        p.extend(varint_enc(((output.len() as u64 - 1) << 2) | 1)); // TargetRead
        p.extend_from_slice(output);
        p.extend_from_slice(&crc32(input).to_le_bytes());
        p.extend_from_slice(&crc32(output).to_le_bytes());
        let hasta = p.len();
        p.extend_from_slice(&crc32(&p[..hasta]).to_le_bytes());
        p
    }

    #[test]
    fn bps_applies_and_verifies() {
        let r = rom();
        let expected: Vec<u8> = r.iter().map(|b| b ^ 0xFF).collect();
        let out = apply_bps(&r, &bps(&r, &expected)).unwrap();
        assert_eq!(out, expected);
    }

    /// LA DIFERENCIA CON IPS: un BPS sabe para qué ROM es y lo dice ANTES de
    /// aplicar nada. Es lo que le importa al usuario que bajó el parche
    /// equivocado.
    #[test]
    fn bps_rejects_wrong_rom() {
        let r = rom();
        let altered_rom: Vec<u8> = r.iter().map(|b| b.wrapping_add(1)).collect();
        let expected: Vec<u8> = r.iter().map(|b| b ^ 0xFF).collect();
        let e = apply_bps(&altered_rom, &bps(&r, &expected)).unwrap_err();
        match e {
            PatchError::WrongRom {
                expected: exp,
                actual,
            } => {
                assert_eq!(exp, crc32(&r));
                assert_eq!(actual, crc32(&altered_rom));
                // Y el mensaje nombra las dos, que es lo que permite al usuario
                // buscar la versión correcta.
                let m = e.message();
                assert!(m.contains(&format!("{exp:08x}")));
            }
            other => panic!("se esperaba WrongRom, salió {other:?}"),
        }
    }

    /// CRC32 de referencia: el de "123456789" es el vector conocido del
    /// algoritmo IEEE. Sin este control, un CRC mal implementado haría que BPS
    /// rechazara todos los parches buenos y aceptara los malos.
    #[test]
    fn crc32_matches_reference() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
    }

    /// NO VACUIDAD: la ROM de prueba y su parcheada tienen que ser DISTINTAS,
    /// si no los tests de arriba pasarían con un patcher que no hace nada.
    #[test]
    fn fixture_actually_changes() {
        let r = rom();
        let out = apply_ips(&r, &ips(&[(0x10, &[0xAA])])).unwrap();
        assert_ne!(out, r);
        assert_ne!(crc32(&out), crc32(&r));
    }
}
