//! Decoders for Mega Drive Game Genie and Pro Action Replay codes.
//!
//! The module validates user-provided codes and returns explicit memory writes;
//! applying those writes remains the caller's responsibility.

// cheat_code.rs — códigos Game Genie y PAR para el JUGADOR.
//
// # De quién es esto
//
// Del jugador, no del modder. El Maper ya deja escribir memoria por dirección
// —es una herramienta de autoría, con su mapa y su modelo— y esto es otra
// cosa: pegar «vidas infinitas» de una revista de 1993 y jugar.
//
// La diferencia no es cosmética. El modder sabe qué dirección toca y por qué;
// el jugador tiene una cadena de nueve letras y ninguna forma de saber si la
// escribió bien. Por eso acá el trabajo está en **decodificar y rechazar**,
// no en aplicar: aplicar ya lo sabe hacer el motor.
//
// # Los dos formatos
//
// **Game Genie de Mega Drive** (`ABCD-EFGH`): ocho símbolos de un alfabeto de
// 32 que codifican 24 bits de dirección y 16 de valor, con los bits
// **barajados** — no es un base32 directo, y ése es exactamente el punto donde
// una implementación se equivoca y produce direcciones plausibles pero
// erróneas, que corrompen la partida sin avisar.
//
// **PAR / Action Replay** (`FFFFFF:0000`): dirección y valor en hexadecimal,
// sin codificar. Trivial de leer y por eso el formato en el que la gente
// comparte códigos nuevos.
//
// # Se aplican por frame, y eso es a propósito
//
// Un cheat no es un parche: el juego reescribe esas direcciones todo el
// tiempo. Escribir una vez sirve para lo que el juego no vuelve a tocar, y
// para lo demás hay que insistir cada frame. Por eso el resultado de decodificar
// es un `(dirección, valor)` que el caller aplica cuando quiera, en vez de una
// escritura ya hecha.

/// A decoded memory write that is ready to apply.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Cheat {
    /// Address in the 68000 address space, not a work-RAM-relative offset.
    pub address: u32,
    /// The 16-bit value to write.
    pub value: u16,
}

/// Reason why a cheat codigo could not be decoded.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CheatError {
    /// The input does not match either supported codigo syntax.
    InvalidFormat,
    /// A Game Genie symbol is not part of the format's alphabet.
    InvalidSymbol(char),
    /// The decoded address is outside cartridge and work RAM address ranges.
    OutOfRange,
    /// The codigo is valid Game Genie syntax, but decoding is not yet supported.
    UnsupportedGameGenie,
}

impl CheatError {
    /// Returns a user-facing explanation of the decoding failure.
    pub fn message(&self) -> String {
        match self {
            CheatError::InvalidFormat => {
                "no parece un codigo: se esperaba ABCD-EFGH (Game Genie) o \
                 FFFFFF:0000 (PAR)"
                    .to_string()
            }
            CheatError::InvalidSymbol(c) => format!(
                "el simbolo '{c}' no existe en un Game Genie — ojo con \
                         I/O/Q/U, que NO se usan justamente para no confundirlas \
                         con 1/0/9/V"
            ),
            CheatError::OutOfRange => {
                "la direccion no es del cartucho: el codigo es para otra consola \
                 o esta mal copiado"
                    .to_string()
            }
            CheatError::UnsupportedGameGenie => {
                "todavia no leemos Game Genie: la tabla de bits del formato no esta \
                 verificada contra codigos conocidos, y una tabla a medias da \
                 direcciones que parecen buenas y rompen la partida. Busca el mismo \
                 truco en formato PAR (FFFFFF:0000), que si funciona"
                    .to_string()
            }
        }
    }
}

/// El alfabeto del Game Genie de Mega Drive, en orden. Los índices SON el
/// valor de 5 bits de cada símbolo.
///
/// Faltan `I O Q U` a propósito y no por olvido: se dejaron afuera porque se
/// confunden con `1 0 9 V` al leer un código de una revista. `S` y `5` SÍ
/// conviven — lo comprobó el test, que primero dijo lo contrario.
const ALPHABET: &[u8; 32] = b"ABCDEFGHJKLMNPRSTVWXYZ0123456789";

fn symbol_value(c: char) -> Result<u32, CheatError> {
    let up = c.to_ascii_uppercase() as u8;
    ALPHABET
        .iter()
        .position(|&x| x == up)
        .map(|i| i as u32)
        .ok_or(CheatError::InvalidSymbol(c))
}

/// Validates a Mega Drive Game Genie codigo (`ABCD-EFGH`).
///
/// The symbol alphabet and input shape are validated, but the bit permutation
/// is intentionally not decoded until verified test vectors are available.
///
/// # Errors
///
/// Returns [`CheatError::InvalidFormat`] for an invalid shape,
/// [`CheatError::InvalidSymbol`] for an unsupported symbol, or
/// [`CheatError::UnsupportedGameGenie`] for valid syntax.
pub fn decode_game_genie(code: &str) -> Result<Cheat, CheatError> {
    // La forma sí se valida: así el usuario sabe que reconocimos QUÉ pegó, y
    // el mensaje puede decirle qué hacer en vez de «formato desconocido».
    let normalized: Vec<char> = code
        .chars()
        .filter(|c| !c.is_whitespace() && *c != '-')
        .collect();
    if normalized.len() != 8 {
        return Err(CheatError::InvalidFormat);
    }
    for c in &normalized {
        symbol_value(*c)?;
    } // y que los símbolos existan
    Err(CheatError::UnsupportedGameGenie)
}

/// Decodes a PAR/Action Replay codigo (`FFFFFF:0000`).
///
/// The separator is optional and hexadecimal digits are case-insensitive.
///
/// # Errors
///
/// Returns [`CheatError::InvalidFormat`] for malformed hexadecimal input or
/// [`CheatError::OutOfRange`] for an unsupported address.
pub fn decode_par(code: &str) -> Result<Cheat, CheatError> {
    let normalized: String = code.chars().filter(|c| !c.is_whitespace()).collect();
    let (a, v) = match normalized.split_once(':') {
        Some(p) => p,
        // Sin `:`, un PAR es 10 hex seguidos: 6 de dirección y 4 de valor.
        None if normalized.len() == 10 => normalized.split_at(6),
        None => return Err(CheatError::InvalidFormat),
    };
    if a.len() != 6 || v.len() != 4 {
        return Err(CheatError::InvalidFormat);
    }
    let addr = u32::from_str_radix(a, 16).map_err(|_| CheatError::InvalidFormat)?;
    let val = u16::from_str_radix(v, 16).map_err(|_| CheatError::InvalidFormat)?;
    // Los PAR de Mega Drive apuntan a work RAM (0xFF0000+) o al cartucho.
    if addr > 0x3F_FFFF && !(0xFF_0000..=0xFF_FFFF).contains(&addr) {
        return Err(CheatError::OutOfRange);
    }
    Ok(Cheat {
        address: addr,
        value: val,
    })
}

/// Detects the codigo syntax and dispatches to the matching decoder.
///
/// # Errors
///
/// Returns the format-specific [`CheatError`] produced by the selected decoder.
pub fn decode(code: &str) -> Result<Cheat, CheatError> {
    let t = code.trim();
    if t.contains(':') || (t.len() == 10 && t.chars().all(|c| c.is_ascii_hexdigit())) {
        return decode_par(t);
    }
    decode_game_genie(t)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// PAR: lo simple, primero. Es el formato en el que circulan los códigos
    /// nuevos y el que se puede verificar leyéndolo.
    #[test]
    fn par_round_trips() {
        assert_eq!(
            decode_par("FFFE21:0009"),
            Ok(Cheat {
                address: 0xFF_FE21,
                value: 0x0009
            })
        );
        // Sin los dos puntos y en minúscula: así circulan.
        assert_eq!(
            decode_par("fffe210009"),
            Ok(Cheat {
                address: 0xFF_FE21,
                value: 0x0009
            })
        );
    }

    /// Una dirección que no es de esta consola se rechaza. Un código de SNES
    /// pegado acá apuntaría a cualquier lado.
    #[test]
    fn par_rejects_out_of_range_address() {
        assert_eq!(decode_par("7E0019:0009"), Err(CheatError::OutOfRange));
        // Pero work RAM sí es válida: es donde viven las vidas y la energía.
        assert!(decode_par("FF0000:0001").is_ok());
        // Y el cartucho también, que es donde pega un Game Genie.
        assert!(decode_par("001234:ABCD").is_ok());
    }

    /// El alfabeto deja afuera I, O, Q y U: se confunden con 1, 0, 9 y V al
    /// leer un código impreso. Un decoder que las aceptara produciría una
    /// dirección plausible y equivocada.
    ///
    /// Son CUATRO y no cinco. La primera versión de este test decía que `S`
    /// también faltaba —por analogía con el `5`— y falló: `S` está en el
    /// alfabeto real. Es la clase de detalle que sólo se puede afirmar
    /// mirando, y por eso el test vale más que el comentario.
    #[test]
    fn alphabet_avoids_ambiguous_characters() {
        for c in ['I', 'O', 'Q', 'U'] {
            assert!(!ALPHABET.contains(&(c as u8)), "{c} no deberia estar");
        }
        assert_eq!(ALPHABET.len(), 32, "32 simbolos = 5 bits exactos");
        // Y no se repite ninguno: dos símbolos con el mismo valor harían
        // ambiguo el decode.
        let mut v = ALPHABET.to_vec();
        v.sort_unstable();
        v.dedup();
        assert_eq!(v.len(), 32);
    }

    /// Un símbolo inválido se NOMBRA — y esto se comprueba ANTES de rechazar
    /// por no soportado: si el usuario escribió mal el código, saberlo le
    /// sirve igual. «Código inválido» a secas deja al usuario revisando ocho
    /// letras de a una.
    #[test]
    fn invalid_symbol_identifies_character() {
        let e = decode_game_genie("ABCD-EFGI").unwrap_err();
        assert_eq!(e, CheatError::InvalidSymbol('I'));
        assert!(e.message().contains('I'));
        // Y el mensaje explica POR QUÉ falta esa letra, que es lo que evita que
        // el usuario la vuelva a escribir.
        assert!(e.message().contains("I/O/Q/U"));
    }

    /// La forma se valida antes que el contenido: ocho símbolos, con guión o
    /// sin él.
    #[test]
    fn shape_is_validated_first() {
        assert_eq!(decode_game_genie("ABC"), Err(CheatError::InvalidFormat));
        assert_eq!(
            decode_game_genie("ABCD-EFGHI"),
            Err(CheatError::InvalidFormat)
        );
        // Con guión y sin guión son el mismo código.
        assert_eq!(
            decode_game_genie("ABCD-EFGH"),
            decode_game_genie("ABCDEFGH")
        );
    }

    /// El formato se detecta solo: nadie sabe si lo que copió es Game Genie o
    /// PAR, y preguntárselo es pedirle que sepa algo que no cambia nada.
    #[test]
    fn format_is_autodetected() {
        assert!(decode("FFFE21:0009").is_ok());
        // Un Game Genie bien formado se RECONOCE aunque no se decodifique: el
        // mensaje puede decir qué hacer en vez de «formato desconocido».
        assert_eq!(decode("ABCD-EFGH"), Err(CheatError::UnsupportedGameGenie));
        assert_eq!(decode("no es un codigo"), Err(CheatError::InvalidFormat));
    }

    /// Y el rechazo EXPLICA y ofrece salida. Un «no soportado» sin más deja al
    /// usuario creyendo que el programa está roto.
    #[test]
    fn game_genie_rejection_suggests_alternative() {
        let m = CheatError::UnsupportedGameGenie.message();
        assert!(m.contains("PAR"), "dice el formato que si funciona");
        assert!(m.contains("verificada"), "y por que no esta");
    }
}
