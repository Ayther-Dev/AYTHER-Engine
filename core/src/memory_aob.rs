//! Array-of-bytes scanning with wildcard support.
//!
//! Patterns locate RAM structures by content instead of fixed addresses, which
//! makes pack logic more resilient across ROM revisions and regions.

// AOB (Array of Bytes) scanner.
// Locates RAM variables by byte-pattern instead of hardcoded addresses,
// making packs robust across ROM versions, translations, and regions.
//
// Wildcards (??) match any byte, e.g. "A9 01 8D ?? ??" locates a LDA/STA
// sequence regardless of the target address that was compiled into the ROM.

/// Parsed byte signature in which `??` tokens match any byte.
pub struct AobPattern {
    bytes: Vec<Option<u8>>,
}

impl AobPattern {
    /// Parse a space-separated hex signature, e.g. `"A9 01 8D ?? ??"`.
    pub fn parse(signature: &str) -> Option<Self> {
        let bytes: Vec<Option<u8>> = signature
            .split_whitespace()
            .map(|token| {
                if token == "??" {
                    None
                } else {
                    u8::from_str_radix(token, 16).ok()
                }
            })
            .collect();

        if bytes.is_empty() {
            return None;
        }
        Some(Self { bytes })
    }

    /// Scan `ram` for the first occurrence of the pattern. Returns byte offset.
    pub fn scan(&self, ram: &[u8]) -> Option<usize> {
        let pattern_len = self.bytes.len();
        'outer: for i in 0..=ram.len().saturating_sub(pattern_len) {
            for (j, expected) in self.bytes.iter().enumerate() {
                if let Some(b) = expected
                    && ram[i + j] != *b
                {
                    continue 'outer;
                }
            }
            return Some(i);
        }
        None
    }

    /// Scans a logical address space supplied by `read` and returns the first
    /// matching address.
    ///
    /// Use this variant when the backing buffer is not stored in address order,
    /// such as word-swapped 68k work RAM on a little-endian host.
    pub fn scan_with<F>(&self, len: u32, read: F) -> Option<u32>
    where
        F: Fn(u32) -> Option<u8>,
    {
        let plen = self.bytes.len() as u32;
        if plen == 0 || len < plen {
            return None;
        }
        'outer: for addr in 0..=(len - plen) {
            for (j, expected) in self.bytes.iter().enumerate() {
                if let Some(b) = expected {
                    match read(addr + j as u32) {
                        Some(v) if v == *b => {}
                        _ => continue 'outer,
                    }
                }
            }
            return Some(addr);
        }
        None
    }

    /// Returns the signature length, including wildcard bytes.
    pub fn len(&self) -> usize {
        self.bytes.len()
    }
    /// Returns whether the signature contains no bytes.
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wildcard_scan() {
        let ram = &[0x00, 0xA9, 0x01, 0x8D, 0x42, 0xFF];
        let pattern = AobPattern::parse("A9 01 8D ?? ??").unwrap();
        assert_eq!(pattern.scan(ram), Some(1));
    }

    #[test]
    fn no_match() {
        let ram = &[0x00, 0x01, 0x02];
        let pattern = AobPattern::parse("FF FF").unwrap();
        assert_eq!(pattern.scan(ram), None);
    }

    #[test]
    fn scan_with_returns_address_not_buffer_index() {
        // Buffer word-swapped (como lo entrega libretro para el 68k): la
        // dirección `a` vive en `raw[a ^ 1]`. El patrón A9 01 8D está en las
        // direcciones 1,2,3 pero NO es contiguo en el buffer crudo.
        let raw = [0xA9u8, 0x00, 0x8D, 0x01, 0xFF, 0x42];
        //          a=1     a=0    a=3    a=2    a=5    a=4
        let read = |a: u32| raw.get((a ^ 1) as usize).copied();
        let pattern = AobPattern::parse("A9 01 8D").unwrap();
        assert_eq!(pattern.scan_with(raw.len() as u32, read), Some(1));
        // El escaneo lineal sobre el mismo buffer no lo encuentra: es el bug
        // que esta variante evita.
        assert_eq!(pattern.scan(&raw), None);
    }

    #[test]
    fn scan_with_honours_wildcards_and_bounds() {
        let ram = [0x11u8, 0x22, 0x33, 0x44];
        let read = |a: u32| ram.get(a as usize).copied();
        assert_eq!(
            AobPattern::parse("22 ?? 44").unwrap().scan_with(4, read),
            Some(1)
        );
        assert_eq!(
            AobPattern::parse("33 44 55").unwrap().scan_with(4, read),
            None
        );
        assert_eq!(AobPattern::parse("11").unwrap().scan_with(0, read), None);
    }
}
