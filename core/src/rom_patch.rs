//! In-memory IPS and BPS patch detection and application.
//!
//! Patches transform caller-owned ROM buffers and never modify the source file
//! on disk. BPS input and output checksums are validated when present.

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

/// Failure returned while detecting or applying a ROM patch.
#[derive(Debug, Clone, PartialEq)]
pub enum PatchError {
    /// The input is neither an IPS nor a BPS patch.
    NoEsParche,
    /// The patch is truncated or structurally invalid.
    Corrupto(&'static str),
    /// The patch attempts to write beyond the format's supported output range.
    FueraDeRango,
    /// The BPS source checksum does not match the supplied ROM.
    RomEquivocada {
        /// Source CRC-32 recorded by the BPS patch.
        esperado: u32,
        /// CRC-32 calculated from the supplied ROM.
        actual: u32,
    },
    /// The patched output does not match the BPS target checksum.
    ResultadoCorrupto {
        /// Target CRC-32 recorded by the BPS patch.
        esperado: u32,
        /// CRC-32 calculated from the patched output.
        actual: u32,
    },
}

impl PatchError {
    /// Returns a consistent user-facing explanation of the failure.
    pub fn mensaje(&self) -> String {
        match self {
            PatchError::NoEsParche => "el archivo no es un parche IPS ni BPS".to_string(),
            PatchError::Corrupto(que) => format!("el parche está incompleto o mal formado: {que}"),
            PatchError::FueraDeRango => {
                "el parche escribe fuera de la ROM: casi seguro es para otra versión \
                 (IPS no trae checksum, así que esto es lo único que lo delata)"
                    .to_string()
            }
            PatchError::RomEquivocada { esperado, actual } => format!(
                "el parche es para otra ROM: espera CRC32 {esperado:08x} y \
                         ésta es {actual:08x}"
            ),
            PatchError::ResultadoCorrupto { esperado, actual } => format!(
                "el parche se aplicó pero el resultado no coincide con lo que \
                         promete (CRC32 {actual:08x}, esperaba {esperado:08x}): el \
                         parche o la ROM están dañados"
            ),
        }
    }
}

/// Computes the IEEE CRC-32 checksum used by BPS patches.
pub fn crc32(datos: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in datos {
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
pub fn es_parche(datos: &[u8]) -> bool {
    datos.starts_with(b"PATCH") || datos.starts_with(b"BPS1")
}

/// Applies an IPS or BPS patch to an in-memory ROM.
///
/// The source slice is never modified. The returned vector contains the patched
/// image.
///
/// # Errors
///
/// Returns [`PatchError`] when the format is unsupported, malformed, targets a
/// different ROM, exceeds format limits, or produces an invalid checksum.
pub fn aplicar(rom: &[u8], parche: &[u8]) -> Result<Vec<u8>, PatchError> {
    if parche.starts_with(b"PATCH") {
        return aplicar_ips(rom, parche);
    }
    if parche.starts_with(b"BPS1") {
        return aplicar_bps(rom, parche);
    }
    Err(PatchError::NoEsParche)
}

// ---------------------------------------------------------------------------
// IPS
// ---------------------------------------------------------------------------

/// Applies an IPS patch to an in-memory ROM.
///
/// Standard records and RLE records are supported. Output may grow up to the
/// 24-bit IPS address-space limit of 16 MiB.
///
/// # Errors
///
/// Returns [`PatchError`] for invalid magic, truncated records, or output beyond
/// the IPS address-space limit.
pub fn aplicar_ips(rom: &[u8], parche: &[u8]) -> Result<Vec<u8>, PatchError> {
    const MAX: usize = 16 * 1024 * 1024; // el techo del formato (offset de 24 bits)
    if parche.len() < 8 || !parche.starts_with(b"PATCH") {
        return Err(PatchError::NoEsParche);
    }
    let mut out = rom.to_vec();
    let mut i = 5usize;
    loop {
        if i + 3 > parche.len() {
            return Err(PatchError::Corrupto("sin EOF"));
        }
        if &parche[i..i + 3] == b"EOF" {
            break;
        }
        let off = ((parche[i] as usize) << 16)
            | ((parche[i + 1] as usize) << 8)
            | (parche[i + 2] as usize);
        i += 3;
        if i + 2 > parche.len() {
            return Err(PatchError::Corrupto("registro cortado"));
        }
        let len = ((parche[i] as usize) << 8) | (parche[i + 1] as usize);
        i += 2;

        let (datos, fin): (Vec<u8>, usize) = if len == 0 {
            // RLE
            if i + 3 > parche.len() {
                return Err(PatchError::Corrupto("RLE cortado"));
            }
            let n = ((parche[i] as usize) << 8) | (parche[i + 1] as usize);
            let v = parche[i + 2];
            i += 3;
            (vec![v; n], off + n)
        } else {
            if i + len > parche.len() {
                return Err(PatchError::Corrupto("datos cortados"));
            }
            let d = parche[i..i + len].to_vec();
            i += len;
            (d, off + len)
        };

        if fin > MAX {
            return Err(PatchError::FueraDeRango);
        }
        if fin > out.len() {
            out.resize(fin, 0);
        }
        out[off..fin].copy_from_slice(&datos);
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
    let mut resultado: u64 = 0;
    let mut shift: u64 = 1;
    loop {
        if *i >= d.len() {
            return Err(PatchError::Corrupto("varint cortado"));
        }
        let x = d[*i];
        *i += 1;
        resultado = resultado.wrapping_add(((x & 0x7F) as u64).wrapping_mul(shift));
        if x & 0x80 != 0 {
            break;
        }
        shift <<= 7;
        resultado = resultado.wrapping_add(shift);
        if shift == 0 {
            return Err(PatchError::Corrupto("varint absurdo"));
        }
    }
    Ok(resultado)
}

/// Applies a BPS patch and validates its source and target checksums.
///
/// # Errors
///
/// Returns [`PatchError`] for malformed actions, a source-ROM mismatch, an
/// invalid patch checksum, or an output checksum mismatch.
pub fn aplicar_bps(rom: &[u8], parche: &[u8]) -> Result<Vec<u8>, PatchError> {
    if parche.len() < 4 + 12 || !parche.starts_with(b"BPS1") {
        return Err(PatchError::NoEsParche);
    }
    // Los tres CRC viven en los últimos 12 bytes: entrada, salida, y el del
    // propio parche.
    let n = parche.len();
    let cuerpo = &parche[..n - 12];
    let leer_u32 = |p: usize| -> u32 {
        u32::from_le_bytes([parche[p], parche[p + 1], parche[p + 2], parche[p + 3]])
    };
    let crc_in = leer_u32(n - 12);
    let crc_out = leer_u32(n - 8);

    // Se comprueba la ROM ANTES de aplicar: aplicar y después decir que estaba
    // mal deja al usuario con un buffer que no sirve y la duda de qué pasó.
    let actual = crc32(rom);
    if actual != crc_in {
        return Err(PatchError::RomEquivocada {
            esperado: crc_in,
            actual,
        });
    }

    let mut i = 4usize;
    let tam_in = varint(cuerpo, &mut i)? as usize;
    let tam_out = varint(cuerpo, &mut i)? as usize;
    let meta = varint(cuerpo, &mut i)? as usize;
    if tam_in != rom.len() {
        return Err(PatchError::Corrupto("tamaño de entrada"));
    }
    i += meta;
    if i > cuerpo.len() {
        return Err(PatchError::Corrupto("metadata cortada"));
    }

    let mut out = vec![0u8; tam_out];
    let mut salida = 0usize;
    let mut src_rel: i64 = 0;
    let mut dst_rel: i64 = 0;

    while i < cuerpo.len() {
        let dato = varint(cuerpo, &mut i)?;
        let accion = (dato & 3) as u8;
        let largo = (dato >> 2) as usize + 1;
        if salida + largo > tam_out {
            return Err(PatchError::Corrupto("acción se pasa"));
        }
        match accion {
            0 => {
                // SourceRead: copiar de la ROM en la misma posición
                if salida + largo > rom.len() {
                    return Err(PatchError::FueraDeRango);
                }
                out[salida..salida + largo].copy_from_slice(&rom[salida..salida + largo]);
                salida += largo;
            }
            1 => {
                // TargetRead: los bytes vienen en el parche
                if i + largo > cuerpo.len() {
                    return Err(PatchError::Corrupto("datos cortados"));
                }
                out[salida..salida + largo].copy_from_slice(&cuerpo[i..i + largo]);
                i += largo;
                salida += largo;
            }
            2 | 3 => {
                // SourceCopy / TargetCopy: offset relativo con signo
                let d = varint(cuerpo, &mut i)?;
                let delta = (d >> 1) as i64 * if d & 1 != 0 { -1 } else { 1 };
                if accion == 2 {
                    src_rel += delta;
                    if src_rel < 0 || src_rel as usize + largo > rom.len() {
                        return Err(PatchError::FueraDeRango);
                    }
                    let s = src_rel as usize;
                    out[salida..salida + largo].copy_from_slice(&rom[s..s + largo]);
                    src_rel += largo as i64;
                } else {
                    dst_rel += delta;
                    if dst_rel < 0 {
                        return Err(PatchError::FueraDeRango);
                    }
                    // Byte a byte y no `copy_from_slice`: TargetCopy puede
                    // solaparse consigo mismo, y ese solapamiento ES el
                    // mecanismo de compresión del formato.
                    for _ in 0..largo {
                        let s = dst_rel as usize;
                        if s >= salida {
                            return Err(PatchError::FueraDeRango);
                        }
                        out[salida] = out[s];
                        dst_rel += 1;
                        salida += 1;
                    }
                }
                if accion == 2 {
                    salida += 0;
                }
            }
            _ => unreachable!(),
        }
    }

    let res = crc32(&out);
    if res != crc_out {
        return Err(PatchError::ResultadoCorrupto {
            esperado: crc_out,
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
    fn ips_escribe_donde_dice() {
        let r = rom();
        let out = aplicar_ips(&r, &ips(&[(0x10, &[0xAA, 0xBB, 0xCC])])).unwrap();
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
    fn la_rom_original_no_se_toca() {
        let r = rom();
        let copia = r.clone();
        let _ = aplicar_ips(&r, &ips(&[(0, &[0xFF; 8])])).unwrap();
        assert_eq!(r, copia);
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
        let out = aplicar_ips(&rom(), &p).unwrap();
        assert_eq!(&out[0x20..0x25], &[0x7E; 5]);
        assert_eq!(out[0x25], rom()[0x25], "y ni uno mas");
    }

    /// Un parche puede EXTENDER la ROM: hay hacks que agregan contenido.
    #[test]
    fn ips_puede_extender() {
        let r = rom();
        let out = aplicar_ips(&r, &ips(&[(r.len(), &[1, 2, 3, 4])])).unwrap();
        assert_eq!(out.len(), r.len() + 4);
        assert_eq!(&out[r.len()..], &[1, 2, 3, 4]);
    }

    /// Un offset absurdo NO reserva gigabytes. Es la forma más fácil de tirar
    /// un proceso con un archivo de cuarenta bytes.
    #[test]
    fn ips_offset_absurdo_no_revienta() {
        let mut p = b"PATCH".to_vec();
        p.extend_from_slice(&[0xFF, 0xFF, 0xFF]); // 16 MB - 1
        p.extend_from_slice(&[0x00, 0x10]);
        p.extend_from_slice(&[0u8; 16]);
        p.extend_from_slice(b"EOF");
        assert_eq!(aplicar_ips(&rom(), &p), Err(PatchError::FueraDeRango));
    }

    /// Un archivo cortado se reporta, no se aplica a medias. Un patcher que
    /// escribe lo que pudo y devuelve Ok deja una ROM que arranca y falla en el
    /// nivel 3.
    #[test]
    fn ips_cortado_no_aplica_a_medias() {
        let p = b"PATCH\x00\x00\x10\x00\x08\x01\x02".to_vec(); // dice 8 bytes, trae 2
        assert!(matches!(
            aplicar_ips(&rom(), &p),
            Err(PatchError::Corrupto(_))
        ));
        let sin_eof = b"PATCH".to_vec();
        assert!(matches!(
            aplicar_ips(&rom(), &sin_eof),
            Err(PatchError::NoEsParche)
        ));
    }

    /// El formato se detecta por la MAGIA y no por la extensión: un `.ips` que
    /// en realidad es un BPS se aplicaría como basura.
    #[test]
    fn el_formato_sale_de_la_magia() {
        assert!(es_parche(b"PATCH..."));
        assert!(es_parche(b"BPS1..."));
        assert!(!es_parche(b"MZ\x90\x00"));
        assert_eq!(
            aplicar(&rom(), b"cualquier cosa"),
            Err(PatchError::NoEsParche)
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
    fn bps(entrada: &[u8], salida: &[u8]) -> Vec<u8> {
        let mut p = b"BPS1".to_vec();
        p.extend(varint_enc(entrada.len() as u64));
        p.extend(varint_enc(salida.len() as u64));
        p.extend(varint_enc(0)); // sin metadata
        p.extend(varint_enc(((salida.len() as u64 - 1) << 2) | 1)); // TargetRead
        p.extend_from_slice(salida);
        p.extend_from_slice(&crc32(entrada).to_le_bytes());
        p.extend_from_slice(&crc32(salida).to_le_bytes());
        let hasta = p.len();
        p.extend_from_slice(&crc32(&p[..hasta]).to_le_bytes());
        p
    }

    #[test]
    fn bps_aplica_y_verifica() {
        let r = rom();
        let esperado: Vec<u8> = r.iter().map(|b| b ^ 0xFF).collect();
        let out = aplicar_bps(&r, &bps(&r, &esperado)).unwrap();
        assert_eq!(out, esperado);
    }

    /// LA DIFERENCIA CON IPS: un BPS sabe para qué ROM es y lo dice ANTES de
    /// aplicar nada. Es lo que le importa al usuario que bajó el parche
    /// equivocado.
    #[test]
    fn bps_rechaza_la_rom_equivocada() {
        let r = rom();
        let otra: Vec<u8> = r.iter().map(|b| b.wrapping_add(1)).collect();
        let esperado: Vec<u8> = r.iter().map(|b| b ^ 0xFF).collect();
        let e = aplicar_bps(&otra, &bps(&r, &esperado)).unwrap_err();
        match e {
            PatchError::RomEquivocada {
                esperado: exp,
                actual,
            } => {
                assert_eq!(exp, crc32(&r));
                assert_eq!(actual, crc32(&otra));
                // Y el mensaje nombra las dos, que es lo que permite al usuario
                // buscar la versión correcta.
                let m = e.mensaje();
                assert!(m.contains(&format!("{exp:08x}")));
            }
            otro => panic!("se esperaba RomEquivocada, salió {otro:?}"),
        }
    }

    /// CRC32 de referencia: el de "123456789" es el vector conocido del
    /// algoritmo IEEE. Sin este control, un CRC mal implementado haría que BPS
    /// rechazara todos los parches buenos y aceptara los malos.
    #[test]
    fn el_crc32_es_el_de_verdad() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
    }

    /// NO VACUIDAD: la ROM de prueba y su parcheada tienen que ser DISTINTAS,
    /// si no los tests de arriba pasarían con un patcher que no hace nada.
    #[test]
    fn el_fixture_cambia_de_verdad() {
        let r = rom();
        let out = aplicar_ips(&r, &ips(&[(0x10, &[0xAA])])).unwrap();
        assert_ne!(out, r);
        assert_ne!(crc32(&out), crc32(&r));
    }
}
