//! Declarative runtime conditions for asset substitutions.
//!
//! Conditions are parsed from pack TOML and evaluated against a reusable
//! [`FrameCtx`]. A condition list uses logical AND; an empty list matches.

// ---------------------------------------------------------------------------
// conditions.rs — evaluador declarativo de condiciones (EM-2 ).
//
// Una sustitución (tile de plano, tile, sprite…) puede llevar una lista de
// condiciones: el mapeo sólo aplica si TODAS dan verdadero (AND lógico). Sin
// condiciones ⇒ comportamiento actual (match por hash, siempre aplica).
//
// Esto resuelve el caso "mismo patrón, objeto distinto según el estado del
// juego" sin llamar al VM de Lua por tile: el predicado es puro y se evalúa
// en Rust contra un `FrameCtx` armado una vez por frame.
//
// ## Subconjunto implementado
//
//   MemoryConst  — compara una dirección de RAM contra una constante
//   MemoryCheck  — compara dos direcciones de RAM entre sí
//   FrameRange   — (frame % divisor) >= at_least   (animación por módulo)
//
// Las variantes espaciales del diseño (`TileNearby`/`SpriteNearby`/`*AtPos`)
// quedan FUERA a propósito: necesitan un índice espacial por frame y ningún
// consumidor las pide todavía. Las de orientación (`HFlip`/`VFlip`/
// `BgPriority`) ya las resuelve `SpriteKey` en `vram_sprite.rs`; formalizarlas
// acá sería duplicar. Cuando entren, se agregan variantes al enum y el resto
// (TOML, catálogo, orden de match) no cambia.
//
// ## TOML
//
//   [[sub]]
//   hash  = "0x1a2b..."
//   asset = "graphics/columna.png"        # sin condiciones ⇒ match global
//
//   [[sub]]
//   hash  = "0x1a2b..."                   # MISMO hash, otro asset…
//   asset = "graphics/columna_noche.png"
//   [[sub.condition]]
//   kind  = "memory_const"                # …desambiguado por estado de juego
//   addr  = "0xFF7E26"                    # o { aob = "A9 ?? 8D ?? ??", offset = 3 }
//   width = "u8"                          # u8 (default) | u16 | u32, big-endian 68k
//   op    = ">="
//   value = 4
//   mask  = "0xFF"                        # opcional
//
// El ORDEN de los `[[sub]]` es significativo: primer match gana.
//
// ## Word-swap (gotcha del proyecto)
//
// La Work RAM del 68k llega word-swapped desde libretro en hosts
// little-endian (el byte de la dirección `a` vive en `bytes[a ^ 1]`). El
// evaluador NO adivina: `RamView` lo declara explícitamente
// (`RamView::word_swapped` vs `RamView::linear`), así una dirección escrita en
// el TOML coincide con la que muestran RetroAchievements / Data Crystal.
// ---------------------------------------------------------------------------

use std::cell::RefCell;
use std::collections::HashMap;

use crate::memory_aob::AobPattern;

// ---------------------------------------------------------------------------
// Vista de RAM
// ---------------------------------------------------------------------------

/// Address-based view of game RAM, with optional 68k word swapping.
///
/// Out-of-range reads return `None`, causing the condition to fail without
/// panicking the engine.
#[derive(Clone, Copy)]
pub struct RamView<'a> {
    bytes: &'a [u8],
    swapped: bool,
}

impl<'a> RamView<'a> {
    /// Wraps RAM that is already stored in 68k address order.
    pub fn linear(bytes: &'a [u8]) -> Self {
        Self {
            bytes,
            swapped: false,
        }
    }

    /// Wraps libretro-style 68k RAM on a little-endian host, where address `a`
    /// is stored at `bytes[a ^ 1]`.
    pub fn word_swapped(bytes: &'a [u8]) -> Self {
        Self {
            bytes,
            swapped: true,
        }
    }

    /// Creates an empty view in which every memory read fails.
    ///
    /// Frame-only conditions continue to work with this view.
    pub fn empty() -> Self {
        Self {
            bytes: &[],
            swapped: false,
        }
    }

    /// Returns the number of bytes in the backing RAM buffer.
    pub fn len(&self) -> usize {
        self.bytes.len()
    }
    /// Returns whether the backing RAM buffer is empty.
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// Returns the raw backing bytes without address translation.
    pub fn raw(&self) -> &'a [u8] {
        self.bytes
    }

    /// Reads one byte at a logical 68k address.
    pub fn u8(&self, addr: u32) -> Option<u8> {
        let i = if self.swapped {
            (addr ^ 1) as usize
        } else {
            addr as usize
        };
        self.bytes.get(i).copied()
    }

    /// Reads a big-endian 16-bit value in native 68k byte order.
    pub fn u16(&self, addr: u32) -> Option<u16> {
        let hi = self.u8(addr)? as u16;
        let lo = self.u8(addr.checked_add(1)?)? as u16;
        Some((hi << 8) | lo)
    }

    /// Reads a big-endian 32-bit value in native 68k byte order.
    pub fn u32(&self, addr: u32) -> Option<u32> {
        let hi = self.u16(addr)? as u32;
        let lo = self.u16(addr.checked_add(2)?)? as u32;
        Some((hi << 16) | lo)
    }

    /// Reads an unsigned value of the selected width.
    pub fn read(&self, addr: u32, width: MemWidth) -> Option<u32> {
        match width {
            MemWidth::U8 => self.u8(addr).map(|v| v as u32),
            MemWidth::U16 => self.u16(addr).map(|v| v as u32),
            MemWidth::U32 => self.u32(addr),
        }
    }
}

// ---------------------------------------------------------------------------
// Contexto de frame
// ---------------------------------------------------------------------------

/// State visible to conditions during one emulated frame.
///
/// Array-of-bytes lookups are cached so repeated conditions sharing a pattern
/// scan RAM only once per context.
pub struct FrameCtx<'a> {
    /// Monotonic frame number used by time-based conditions.
    pub frame_number: u64,
    /// Game RAM visible during this frame.
    pub ram: RamView<'a>,
    aob_cache: RefCell<HashMap<String, Option<u32>>>,
}

impl<'a> FrameCtx<'a> {
    /// Creates a frame context with a RAM view.
    pub fn new(frame_number: u64, ram: RamView<'a>) -> Self {
        Self {
            frame_number,
            ram,
            aob_cache: RefCell::new(HashMap::new()),
        }
    }

    /// Creates a context without RAM for frame-only conditions.
    pub fn frame_only(frame_number: u64) -> Self {
        Self::new(frame_number, RamView::empty())
    }

    /// Resolves a memory reference for this frame, caching pattern scans.
    pub fn resolve_addr(&self, m: &MemRef) -> Option<u32> {
        match m {
            MemRef::Abs(a) => Some(*a),
            MemRef::Aob { pattern, offset } => {
                if let Some(hit) = self.aob_cache.borrow().get(pattern) {
                    return apply_offset(*hit, *offset);
                }
                // Escaneo en espacio de DIRECCIONES (no de buffer): con la RAM
                // word-swapped del 68k, los bytes del patrón no son contiguos
                // en el buffer crudo y el índice tampoco sería la dirección.
                let len = u32::try_from(self.ram.len()).unwrap_or(u32::MAX);
                let found =
                    AobPattern::parse(pattern).and_then(|p| p.scan_with(len, |a| self.ram.u8(a)));
                self.aob_cache.borrow_mut().insert(pattern.clone(), found);
                apply_offset(found, *offset)
            }
        }
    }
}

fn apply_offset(base: Option<u32>, offset: i32) -> Option<u32> {
    let b = base?;
    if offset >= 0 {
        b.checked_add(offset as u32)
    } else {
        b.checked_sub(offset.unsigned_abs())
    }
}

// ---------------------------------------------------------------------------
// Tipos del predicado
// ---------------------------------------------------------------------------

/// Unsigned comparison operator used by memory conditions.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CmpOp {
    /// Equal.
    Eq,
    /// Not equal.
    Ne,
    /// Greater than.
    Gt,
    /// Less than.
    Lt,
    /// Greater than or equal.
    Ge,
    /// Less than or equal.
    Le,
}

impl CmpOp {
    /// Applies the comparison to two unsigned values.
    pub fn apply(self, lhs: u32, rhs: u32) -> bool {
        match self {
            CmpOp::Eq => lhs == rhs,
            CmpOp::Ne => lhs != rhs,
            CmpOp::Gt => lhs > rhs,
            CmpOp::Lt => lhs < rhs,
            CmpOp::Ge => lhs >= rhs,
            CmpOp::Le => lhs <= rhs,
        }
    }

    /// Parses a symbolic or mnemonic comparison operator.
    pub fn parse(s: &str) -> Option<Self> {
        Some(match s.trim() {
            "=" | "==" | "eq" => CmpOp::Eq,
            "!=" | "<>" | "ne" => CmpOp::Ne,
            ">" | "gt" => CmpOp::Gt,
            "<" | "lt" => CmpOp::Lt,
            ">=" | "ge" => CmpOp::Ge,
            "<=" | "le" => CmpOp::Le,
            _ => return None,
        })
    }
}

/// Width of a memory value read by a condition.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum MemWidth {
    /// Eight-bit value; the default.
    #[default]
    U8,
    /// Sixteen-bit big-endian value.
    U16,
    /// Thirty-two-bit big-endian value.
    U32,
}

impl MemWidth {
    /// Parses a width name, bit count, or common mnemonic.
    pub fn parse(s: &str) -> Option<Self> {
        Some(match s.trim().to_ascii_lowercase().as_str() {
            "u8" | "8" | "byte" => MemWidth::U8,
            "u16" | "16" | "word" => MemWidth::U16,
            "u32" | "32" | "long" => MemWidth::U32,
            _ => return None,
        })
    }
}

/// Address source for a memory condition.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MemRef {
    /// Absolute 68k address.
    Abs(u32),
    /// Address located by an array-of-bytes pattern and signed offset.
    Aob {
        /// Space-separated byte pattern with optional `??` wildcards.
        pattern: String,
        /// Signed offset applied to the first pattern match.
        offset: i32,
    },
}

/// Pure predicate over one frame's state.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Condition {
    /// Compares a memory value with a pack-authored constant.
    MemoryConst {
        /// Address to read.
        addr: MemRef,
        /// Width of the value.
        width: MemWidth,
        /// Comparison operator.
        op: CmpOp,
        /// Right-hand constant.
        value: u32,
        /// Bit mask applied to both sides.
        mask: u32,
    },
    /// Compares values read from two addresses.
    MemoryCheck {
        /// Left-hand address.
        addr: MemRef,
        /// Right-hand address.
        rhs: MemRef,
        /// Width of both values.
        width: MemWidth,
        /// Comparison operator.
        op: CmpOp,
        /// Bit mask applied to both sides.
        mask: u32,
    },
    /// Matches when `(frame % divisor) >= at_least`.
    FrameRange {
        /// Length of the repeating frame interval.
        divisor: u32,
        /// First matching remainder within the interval.
        at_least: u32,
    },
}

impl Condition {
    /// Evaluates this condition against a frame context.
    pub fn eval(&self, ctx: &FrameCtx) -> bool {
        match self {
            Condition::MemoryConst {
                addr,
                width,
                op,
                value,
                mask,
            } => {
                match ctx.resolve_addr(addr).and_then(|a| ctx.ram.read(a, *width)) {
                    Some(v) => op.apply(v & mask, *value & mask),
                    None => false, // dirección inválida ⇒ no matchea
                }
            }
            Condition::MemoryCheck {
                addr,
                rhs,
                width,
                op,
                mask,
            } => {
                let l = ctx.resolve_addr(addr).and_then(|a| ctx.ram.read(a, *width));
                let r = ctx.resolve_addr(rhs).and_then(|a| ctx.ram.read(a, *width));
                match (l, r) {
                    (Some(l), Some(r)) => op.apply(l & mask, r & mask),
                    _ => false,
                }
            }
            Condition::FrameRange { divisor, at_least } => {
                if *divisor == 0 {
                    return false;
                }
                (ctx.frame_number % (*divisor as u64)) >= (*at_least as u64)
            }
        }
    }
}

/// Evaluates the logical AND of a condition list.
///
/// An empty list matches globally.
pub fn eval_all(conds: &[Condition], ctx: &FrameCtx) -> bool {
    conds.iter().all(|c| c.eval(ctx))
}

// ---------------------------------------------------------------------------
// Esquema TOML (serde) → Condition
// ---------------------------------------------------------------------------

/// `addr = "0xFF7E26"` · `addr = 16744486` · `addr = { aob = "…", offset = 3 }`
#[derive(serde::Deserialize, Debug)]
#[serde(untagged)]
pub enum MemRefSpec {
    /// Numeric absolute address.
    Num(u32),
    /// String absolute address, normally hexadecimal.
    Hex(String),
    /// Pattern-located address.
    Aob {
        /// Space-separated byte signature.
        aob: String,
        /// Signed offset from the first match.
        #[serde(default)]
        offset: i32,
    },
}

impl MemRefSpec {
    fn build(&self) -> Result<MemRef, String> {
        match self {
            MemRefSpec::Num(n) => Ok(MemRef::Abs(*n)),
            MemRefSpec::Hex(s) => parse_u32_lit(s)
                .map(MemRef::Abs)
                .ok_or_else(|| format!("dirección inválida: {:?}", s)),
            MemRefSpec::Aob { aob, offset } => {
                if AobPattern::parse(aob).is_none() {
                    return Err(format!("patrón AOB inválido: {:?}", aob));
                }
                Ok(MemRef::Aob {
                    pattern: aob.clone(),
                    offset: *offset,
                })
            }
        }
    }
}

/// TOML representation of a numeric value written as a number or string.
#[derive(serde::Deserialize, Debug)]
#[serde(untagged)]
pub enum MaskSpec {
    /// Numeric TOML value.
    Num(u32),
    /// String value, normally hexadecimal.
    Hex(String),
}

impl MaskSpec {
    fn build(&self) -> Result<u32, String> {
        match self {
            MaskSpec::Num(n) => Ok(*n),
            MaskSpec::Hex(s) => {
                parse_u32_lit(s).ok_or_else(|| format!("máscara inválida: {:?}", s))
            }
        }
    }
}

/// Deserialized `[[sub.condition]]` entry.
#[derive(serde::Deserialize, Debug, Default)]
pub struct CondSpec {
    /// Condition kind, such as `memory_const` or `frame_range`.
    pub kind: String,
    /// Primary memory reference.
    pub addr: Option<MemRefSpec>,
    /// Right-hand memory reference for memory-to-memory checks.
    pub rhs: Option<MemRefSpec>,
    /// Comparison operator.
    pub op: Option<String>,
    /// Constant comparison value.
    pub value: Option<MaskSpec>, // mismo parser: número o "0x…"
    /// Optional bit mask.
    pub mask: Option<MaskSpec>,
    /// Memory width; defaults to `u8`.
    pub width: Option<String>,
    /// Modulus for a frame-range condition.
    pub divisor: Option<u32>,
    /// First matching remainder for a frame-range condition.
    pub at_least: Option<u32>,
}

impl CondSpec {
    /// Validates and compiles this deserialized specification.
    ///
    /// # Errors
    ///
    /// Returns a diagnostic for missing, malformed, or unsupported fields.
    pub fn build(&self) -> Result<Condition, String> {
        let kind = self.kind.trim().to_ascii_lowercase().replace('-', "_");
        let width = match &self.width {
            Some(w) => MemWidth::parse(w).ok_or_else(|| format!("width inválido: {:?}", w))?,
            None => MemWidth::U8,
        };
        let mask = match &self.mask {
            Some(m) => m.build()?,
            None => u32::MAX,
        };
        let op = || -> Result<CmpOp, String> {
            let s = self.op.as_deref().ok_or("falta `op`")?;
            CmpOp::parse(s).ok_or_else(|| format!("op inválido: {:?}", s))
        };
        let addr = || -> Result<MemRef, String> {
            self.addr
                .as_ref()
                .ok_or("falta `addr`".to_string())?
                .build()
        };

        match kind.as_str() {
            "memory_const" | "memoryconst" => {
                let value = self.value.as_ref().ok_or("falta `value`")?.build()?;
                Ok(Condition::MemoryConst {
                    addr: addr()?,
                    width,
                    op: op()?,
                    value,
                    mask,
                })
            }
            "memory_check" | "memorycheck" => {
                let rhs = self.rhs.as_ref().ok_or("falta `rhs`")?.build()?;
                Ok(Condition::MemoryCheck {
                    addr: addr()?,
                    rhs,
                    width,
                    op: op()?,
                    mask,
                })
            }
            "frame_range" | "framerange" => {
                let divisor = self.divisor.ok_or("falta `divisor`")?;
                if divisor == 0 {
                    return Err("`divisor` no puede ser 0".into());
                }
                Ok(Condition::FrameRange {
                    divisor,
                    at_least: self.at_least.unwrap_or(0),
                })
            }
            other => Err(format!(
                "condición desconocida: {:?} (soportadas: memory_const, memory_check, frame_range)",
                other
            )),
        }
    }
}

/// Compiles a complete condition list.
///
/// # Errors
///
/// Stops at the first invalid specification and includes its index in the
/// returned diagnostic.
pub fn build_conditions(specs: &[CondSpec]) -> Result<Vec<Condition>, String> {
    let mut out = Vec::with_capacity(specs.len());
    for (i, s) in specs.iter().enumerate() {
        out.push(s.build().map_err(|e| format!("condition[{}]: {}", i, e))?);
    }
    Ok(out)
}

/// "0xFF" / "0XFF" / "255" → u32.
fn parse_u32_lit(s: &str) -> Option<u32> {
    let t = s.trim();
    match t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => t.parse::<u32>().ok(),
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn ram_linear() -> Vec<u8> {
        // addr:  0    1    2    3    4    5
        vec![0x00, 0x04, 0x12, 0x34, 0xFF, 0x0A]
    }

    #[test]
    fn ramview_linear_widths_are_big_endian() {
        let r = ram_linear();
        let v = RamView::linear(&r);
        assert_eq!(v.u8(1), Some(0x04));
        assert_eq!(v.u16(2), Some(0x1234));
        assert_eq!(v.u32(0), Some(0x00041234));
        assert_eq!(v.u8(99), None);
        assert_eq!(v.u16(5), None); // se pasa por un byte
    }

    #[test]
    fn ramview_word_swapped_matches_68k_addresses() {
        // Buffer tal cual lo entrega libretro en LE: el byte de la dirección 0
        // está en el índice 1 y viceversa.
        let raw = vec![0x22u8, 0x11, 0x44, 0x33];
        let v = RamView::word_swapped(&raw);
        assert_eq!(v.u8(0), Some(0x11));
        assert_eq!(v.u8(1), Some(0x22));
        assert_eq!(v.u8(2), Some(0x33));
        assert_eq!(v.u16(0), Some(0x1122));
        assert_eq!(v.u32(0), Some(0x11223344));
        // La vista lineal sobre el mismo buffer daría lo contrario: es
        // exactamente el bug que la distinción explícita evita.
        assert_eq!(RamView::linear(&raw).u16(0), Some(0x2211));
    }

    #[test]
    fn memory_const_compares_with_mask() {
        let r = ram_linear();
        let ctx = FrameCtx::new(0, RamView::linear(&r));
        let c = Condition::MemoryConst {
            addr: MemRef::Abs(4),
            width: MemWidth::U8,
            op: CmpOp::Eq,
            value: 0x0F,
            mask: 0x0F,
        };
        assert!(c.eval(&ctx)); // 0xFF & 0x0F == 0x0F
        let c2 = Condition::MemoryConst {
            addr: MemRef::Abs(4),
            width: MemWidth::U8,
            op: CmpOp::Eq,
            value: 0x0F,
            mask: 0xFF,
        };
        assert!(!c2.eval(&ctx)); // 0xFF != 0x0F
    }

    #[test]
    fn memory_const_out_of_range_is_false_not_panic() {
        let r = ram_linear();
        let ctx = FrameCtx::new(0, RamView::linear(&r));
        let c = Condition::MemoryConst {
            addr: MemRef::Abs(0xFF_FFFF),
            width: MemWidth::U32,
            op: CmpOp::Ge,
            value: 0,
            mask: u32::MAX,
        };
        assert!(!c.eval(&ctx));
    }

    #[test]
    fn memory_check_compares_two_addresses() {
        let r = ram_linear();
        let ctx = FrameCtx::new(0, RamView::linear(&r));
        let c = Condition::MemoryCheck {
            addr: MemRef::Abs(4),
            rhs: MemRef::Abs(1),
            width: MemWidth::U8,
            op: CmpOp::Gt,
            mask: u32::MAX,
        };
        assert!(c.eval(&ctx)); // 0xFF > 0x04
    }

    #[test]
    fn frame_range_is_modular() {
        let c = Condition::FrameRange {
            divisor: 10,
            at_least: 5,
        };
        assert!(!c.eval(&FrameCtx::frame_only(4)));
        assert!(c.eval(&FrameCtx::frame_only(5)));
        assert!(c.eval(&FrameCtx::frame_only(19)));
        assert!(!c.eval(&FrameCtx::frame_only(20)));
    }

    #[test]
    fn frame_range_divisor_zero_is_false() {
        let c = Condition::FrameRange {
            divisor: 0,
            at_least: 0,
        };
        assert!(!c.eval(&FrameCtx::frame_only(7)));
    }

    #[test]
    fn aob_resolves_and_caches() {
        let raw = vec![0x00, 0xA9, 0x01, 0x8D, 0x42, 0x07];
        let ctx = FrameCtx::new(0, RamView::linear(&raw));
        let m = MemRef::Aob {
            pattern: "A9 01 8D ?? ??".into(),
            offset: 4,
        };
        assert_eq!(ctx.resolve_addr(&m), Some(5)); // hit en 1 + offset 4
        assert_eq!(ctx.resolve_addr(&m), Some(5)); // segunda vez: cacheado
        assert_eq!(ctx.aob_cache.borrow().len(), 1);

        let c = Condition::MemoryConst {
            addr: m,
            width: MemWidth::U8,
            op: CmpOp::Eq,
            value: 0x07,
            mask: 0xFF,
        };
        assert!(c.eval(&ctx));
    }

    #[test]
    fn aob_scans_in_address_space_over_swapped_ram() {
        // El caso real: Work RAM word-swapped. El patrón vive en las
        // direcciones 1..3 y el dato objetivo en la 4.
        let raw = [0xA9u8, 0x00, 0x8D, 0x01, 0x07, 0x42];
        //          a=1     a=0    a=3    a=2    a=5    a=4
        let ctx = FrameCtx::new(0, RamView::word_swapped(&raw));
        let m = MemRef::Aob {
            pattern: "A9 01 8D".into(),
            offset: 3,
        };
        assert_eq!(ctx.resolve_addr(&m), Some(4)); // dirección, no índice
        let c = Condition::MemoryConst {
            addr: m,
            width: MemWidth::U8,
            op: CmpOp::Eq,
            value: 0x42,
            mask: 0xFF,
        };
        assert!(c.eval(&ctx)); // ram[a=4] = 0x42
    }

    #[test]
    fn aob_miss_is_false() {
        let raw = vec![0x00, 0x01, 0x02];
        let ctx = FrameCtx::new(0, RamView::linear(&raw));
        let c = Condition::MemoryConst {
            addr: MemRef::Aob {
                pattern: "FF FF FF".into(),
                offset: 0,
            },
            width: MemWidth::U8,
            op: CmpOp::Eq,
            value: 0,
            mask: 0xFF,
        };
        assert!(!c.eval(&ctx));
    }

    #[test]
    fn eval_all_is_and_and_empty_is_true() {
        let r = ram_linear();
        let ctx = FrameCtx::new(3, RamView::linear(&r));
        assert!(eval_all(&[], &ctx));
        let yes = Condition::FrameRange {
            divisor: 2,
            at_least: 1,
        }; // 3%2=1 >= 1
        let no = Condition::FrameRange {
            divisor: 2,
            at_least: 2,
        };
        assert!(eval_all(std::slice::from_ref(&yes), &ctx));
        assert!(!eval_all(&[yes, no], &ctx));
    }

    // -- TOML -------------------------------------------------------------

    fn spec_of(toml_src: &str) -> Vec<CondSpec> {
        #[derive(serde::Deserialize)]
        struct W {
            condition: Vec<CondSpec>,
        }
        toml::from_str::<W>(toml_src).expect("toml").condition
    }

    #[test]
    fn toml_memory_const_hex_addr_and_mask() {
        let specs = spec_of(
            r#"
[[condition]]
kind  = "memory_const"
addr  = "0xFF7E26"
width = "u8"
op    = ">="
value = 4
mask  = "0xFF"
"#,
        );
        let c = build_conditions(&specs).expect("build");
        assert_eq!(
            c,
            vec![Condition::MemoryConst {
                addr: MemRef::Abs(0xFF7E26),
                width: MemWidth::U8,
                op: CmpOp::Ge,
                value: 4,
                mask: 0xFF,
            }]
        );
    }

    #[test]
    fn toml_memory_const_defaults_width_u8_and_full_mask() {
        let specs = spec_of("[[condition]]\nkind=\"memory_const\"\naddr=16\nop=\"=\"\nvalue=1\n");
        match &build_conditions(&specs).expect("build")[0] {
            Condition::MemoryConst {
                width, mask, addr, ..
            } => {
                assert_eq!(*width, MemWidth::U8);
                assert_eq!(*mask, u32::MAX);
                assert_eq!(*addr, MemRef::Abs(16));
            }
            other => panic!("variante inesperada: {:?}", other),
        }
    }

    #[test]
    fn toml_aob_addr_with_offset() {
        let specs = spec_of(
            r#"
[[condition]]
kind  = "memory_check"
addr  = { aob = "A9 ?? 8D ?? ??", offset = 3 }
rhs   = "0x10"
width = "u16"
op    = "!="
"#,
        );
        let c = build_conditions(&specs).expect("build");
        assert_eq!(
            c[0],
            Condition::MemoryCheck {
                addr: MemRef::Aob {
                    pattern: "A9 ?? 8D ?? ??".into(),
                    offset: 3
                },
                rhs: MemRef::Abs(0x10),
                width: MemWidth::U16,
                op: CmpOp::Ne,
                mask: u32::MAX,
            }
        );
    }

    #[test]
    fn toml_frame_range() {
        let specs = spec_of("[[condition]]\nkind=\"frame_range\"\ndivisor=8\nat_least=4\n");
        assert_eq!(
            build_conditions(&specs).expect("build")[0],
            Condition::FrameRange {
                divisor: 8,
                at_least: 4
            }
        );
    }

    #[test]
    fn toml_errors_are_clear_and_indexed() {
        let specs = spec_of(
            "[[condition]]\nkind=\"frame_range\"\ndivisor=8\n\n\
                             [[condition]]\nkind=\"memory_const\"\naddr=1\nvalue=1\n",
        );
        let err = build_conditions(&specs).unwrap_err();
        assert!(err.starts_with("condition[1]:"), "err = {}", err);
        assert!(err.contains("op"), "err = {}", err);

        let bad_kind = spec_of("[[condition]]\nkind=\"tile_nearby\"\n");
        let err = build_conditions(&bad_kind).unwrap_err();
        assert!(err.contains("desconocida"), "err = {}", err);

        let bad_op = spec_of("[[condition]]\nkind=\"memory_const\"\naddr=1\nop=\"~~\"\nvalue=1\n");
        assert!(
            build_conditions(&bad_op)
                .unwrap_err()
                .contains("op inválido")
        );

        let bad_aob = spec_of(
            "[[condition]]\nkind=\"memory_const\"\n\
                               addr={aob=\"\"}\nop=\"=\"\nvalue=1\n",
        );
        assert!(build_conditions(&bad_aob).unwrap_err().contains("AOB"));

        let bad_div = spec_of("[[condition]]\nkind=\"frame_range\"\ndivisor=0\n");
        assert!(build_conditions(&bad_div).unwrap_err().contains("divisor"));
    }

    #[test]
    fn op_aliases_parse() {
        for (s, want) in [
            ("=", CmpOp::Eq),
            ("==", CmpOp::Eq),
            ("!=", CmpOp::Ne),
            ("<>", CmpOp::Ne),
            (">", CmpOp::Gt),
            ("<", CmpOp::Lt),
            (">=", CmpOp::Ge),
            ("<=", CmpOp::Le),
            ("ge", CmpOp::Ge),
        ] {
            assert_eq!(CmpOp::parse(s), Some(want), "op {:?}", s);
        }
        assert_eq!(CmpOp::parse("=>"), None);
    }
}
