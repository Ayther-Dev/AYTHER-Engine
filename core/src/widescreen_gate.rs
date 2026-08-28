//! Conditional selection of authored widescreen output widths.
//!
//! Rules are evaluated in declaration order with [`crate::conditions`], letting
//! packs enable extended scenery only in game states where it is available.

// ---------------------------------------------------------------------------
// widescreen_gate — EM-8.2 de  hasta dónde ensanchar, y bajo qué condición.
//
// POR QUÉ ES OBLIGATORIO Y NO UNA PRECAUCIÓN. El área extendida se dibuja desde
// la lámina del nivel, y la lámina sólo existe donde el juego RECORRIÓ. Está
// medido (`widescreen_spike`): en una toma QUIETA la racha dibujable es 0 por
// los cuatro lados. Un menú, una pantalla de título o un juego que no scrollea
// no tienen arte lateral de ninguna clase — ensanchar ahí no muestra el nivel,
// muestra el vacío. Por eso el gate no es un refinamiento: sin él, el
// ensanchado está roto la mitad del tiempo.
//
// EL EVALUADOR ES EL DE SIEMPRE. `crate::conditions` — el mismo que usan los
// tiles y el gate de audio. Una segunda implementación del
// dialecto se desincronizaría, y el autor tendría que aprender dos.
//
// EL PACK DECLARA UNA LISTA, no un solo ancho. Cada entrada trae su ancho y sus
// condiciones; gana LA PRIMERA que se cumple. Así un pack puede ensanchar a
// fondo en gameplay, a medias en un jefe con la cámara fija y nada en los
// menús, sin que el motor sepa qué es un jefe:
//
//   [[widescreen]]
//   width = 398                      # 16:9 en píxel cuadrado sobre 224 líneas
//   [[widescreen.condition]]
//   kind = "memory_const"
//   addr = "0xFFF600"                # modo de juego
//   op   = "eq"
//   value = 0x0C                     # gameplay
//
//   [[widescreen]]
//   width = 0                        # el resto: 4:3, sin condiciones
//
// SIN ENTRADAS, EL GATE NO EXISTE. `from_toml` de un pack sin `[[widescreen]]`
// da un gate vacío y `width_for` devuelve None — que el caller traduce a «no
// toco nada», no a «apagá el ensanchado». La diferencia importa: los packs ya
// horneados no declaran nada y el ensanchado manual del Lab tiene que seguir
// funcionando.
//
// UNA ENTRADA SIN CONDICIONES ES EL DEFAULT, y va última a propósito: `eval_all`
// de una lista vacía es verdadero, así que una entrada sin condiciones matchea
// siempre y todo lo que venga después queda muerto. Se acepta igual (es la
// forma natural de escribir «el resto»), pero declararla primera desactiva las
// demás — y eso se avisa al compilar, no se descubre mirando el juego.
// ---------------------------------------------------------------------------
use crate::conditions::{CondSpec, Condition, FrameCtx, build_conditions, eval_all};

/// One authored output width and the conditions that enable it.
#[derive(Clone, Debug)]
pub struct WidescreenRule {
    /// Logical emulator width in pixels; zero selects the original 4:3 output.
    pub width: u32,
    /// Conditions that must all match for this rule to apply.
    pub conds: Vec<Condition>,
}

/// Ordered set of conditional widescreen rules.
#[derive(Clone, Debug, Default)]
pub struct WidescreenGate {
    rules: Vec<WidescreenRule>,
}

impl WidescreenGate {
    /// Returns whether the pack declared no widescreen rules.
    pub fn is_empty(&self) -> bool {
        self.rules.is_empty()
    }
    /// Returns the number of compiled rules.
    pub fn len(&self) -> usize {
        self.rules.len()
    }

    /// Compiles rules from `widescreen.toml` or an equivalent TOML section.
    ///
    /// A rule containing a malformed condition is discarded instead of becoming
    /// unconditional. Invalid TOML produces an empty gate.
    pub fn from_toml(text: &str) -> Self {
        let mut rules: Vec<WidescreenRule> = Vec::new();
        let tbl: toml::Value = match toml::from_str(text) {
            Ok(t) => t,
            Err(e) => {
                eprintln!("[WidescreenGate] TOML inválido: {e} — gate vacío");
                return Self { rules };
            }
        };
        let arr = match tbl.get("widescreen").and_then(|v| v.as_array()) {
            Some(a) => a,
            None => return Self { rules },
        };
        for (i, e) in arr.iter().enumerate() {
            // `width` ausente es un error y no un 0 implícito: 0 significa
            // «apagá el ensanchado acá», que es una decisión, y confundirla con
            // un campo olvidado deja un pack que apaga sin que nadie lo pidió.
            let width = match e.get("width").and_then(|v| v.as_integer()) {
                Some(w) if (0..=4096).contains(&w) => w as u32,
                Some(w) => {
                    eprintln!(
                        "[WidescreenGate] widescreen[{i}]: width fuera de rango ({w}) — entrada ignorada"
                    );
                    continue;
                }
                None => {
                    eprintln!("[WidescreenGate] widescreen[{i}]: falta `width` — entrada ignorada");
                    continue;
                }
            };
            let raw = e.get("condition").and_then(|v| v.as_array());
            let conds = match raw {
                Some(r) if !r.is_empty() => {
                    let specs: Vec<CondSpec> = r
                        .iter()
                        .filter_map(|v| v.clone().try_into::<CondSpec>().ok())
                        .collect();
                    if specs.len() != r.len() {
                        eprintln!(
                            "[WidescreenGate] widescreen[{i}]: condición malformada — entrada ignorada"
                        );
                        continue;
                    }
                    match build_conditions(&specs) {
                        Ok(c) => c,
                        Err(msg) => {
                            eprintln!("[WidescreenGate] widescreen[{i}]: {msg} — entrada ignorada");
                            continue;
                        }
                    }
                }
                _ => Vec::new(),
            };
            // Una entrada sin condiciones matchea SIEMPRE, así que todo lo que
            // venga después nunca se evalúa. Es legítima como última («el
            // resto») y casi siempre un error en cualquier otra posición: se
            // avisa acá, al compilar, en vez de dejarlo para que el autor lo
            // descubra viendo que su regla de gameplay no hace nada.
            if conds.is_empty() && i + 1 < arr.len() {
                eprintln!(
                    "[WidescreenGate] widescreen[{i}]: sin condiciones y NO es la última — las {} entradas siguientes quedan muertas",
                    arr.len() - i - 1
                );
            }
            rules.push(WidescreenRule { width, conds });
        }
        Self { rules }
    }

    /// Returns the logical width selected for this frame.
    ///
    /// `None` means the pack has no applicable opinion and the caller should
    /// retain its current setting. `Some(0)` explicitly selects 4:3 output.
    pub fn width_for(&self, ctx: &FrameCtx) -> Option<u32> {
        self.rules
            .iter()
            .find(|r| eval_all(&r.conds, ctx))
            .map(|r| r.width)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::conditions::RamView;

    /// RAM con el «modo de juego» en 0: 0x0C = gameplay, 0x00 = menú.
    fn ctx_with(mode: u8) -> Vec<u8> {
        let mut r = vec![0u8; 16];
        r[0] = mode;
        r
    }

    const TOML: &str = r#"
[[widescreen]]
width = 398
[[widescreen.condition]]
kind = "memory_const"
addr = 0
width = "u8"
op = "eq"
value = 12

[[widescreen]]
width = 0
"#;

    #[test]
    fn gameplay_widens_and_menu_returns_to_4_3() {
        let g = WidescreenGate::from_toml(TOML);
        assert_eq!(g.len(), 2, "las dos reglas compilan");

        let ram = ctx_with(0x0C);
        let ctx = FrameCtx::new(0, RamView::linear(&ram));
        assert_eq!(g.width_for(&ctx), Some(398), "en gameplay ensancha");

        // El AC de EM-8.2: al entrar al menú el ancho vuelve a 4:3. Es el MISMO
        // gate, con la RAM cambiada — nada de estado propio que pueda quedar
        // rancio entre los dos.
        let ram = ctx_with(0x00);
        let ctx = FrameCtx::new(0, RamView::linear(&ram));
        assert_eq!(g.width_for(&ctx), Some(0), "en el menu vuelve a 4:3");
    }

    #[test]
    fn missing_declaration_leaves_gate_undecided() {
        // Un pack ya horneado no trae `[[widescreen]]`. Tiene que dar None y no
        // Some(0): con Some(0) el gate apagaría el ensanchado manual del Lab en
        // todos los packs existentes.
        let g = WidescreenGate::from_toml("[[sub]]\nhash = \"0x1\"\n");
        assert!(g.is_empty());
        assert_eq!(g.width_for(&FrameCtx::frame_only(0)), None);
    }

    #[test]
    fn broken_condition_discards_entry() {
        // Es la trampa que este gate NO puede permitirse: una entrada rota que
        // matchea siempre ensancha en los menús, justo donde no hay lámina.
        let g = WidescreenGate::from_toml(
            r#"
[[widescreen]]
width = 398
[[widescreen.condition]]
kind = "no_existe_esta_condicion"

[[widescreen]]
width = 0
"#,
        );
        assert_eq!(g.len(), 1, "sólo sobrevive la entrada sana");
        assert_eq!(
            g.width_for(&FrameCtx::frame_only(0)),
            Some(0),
            "y la que queda es la de 4:3, no la rota"
        );
    }

    #[test]
    fn first_matching_rule_wins() {
        let g = WidescreenGate::from_toml(
            r#"
[[widescreen]]
width = 427
[[widescreen.condition]]
kind = "memory_const"
addr = 0
width = "u8"
op = "eq"
value = 1

[[widescreen]]
width = 398
[[widescreen.condition]]
kind = "memory_const"
addr = 0
width = "u8"
op = "lt"
value = 16

[[widescreen]]
width = 0
"#,
        );
        // mode = 1 cumple las DOS primeras (eq 1, y lt 16). Gana la primera.
        let ram = ctx_with(1);
        let ctx = FrameCtx::new(0, RamView::linear(&ram));
        assert_eq!(
            g.width_for(&ctx),
            Some(427),
            "gana la primera, no la mas ancha"
        );

        // mode = 2 sólo cumple la segunda.
        let ram = ctx_with(2);
        let ctx = FrameCtx::new(0, RamView::linear(&ram));
        assert_eq!(g.width_for(&ctx), Some(398));
    }

    #[test]
    fn missing_width_is_not_implicit_zero() {
        // Un `width` olvidado tomado como 0 apagaría el ensanchado sin que
        // nadie lo pidiera, y el autor vería «no anda» en vez de un error.
        let g = WidescreenGate::from_toml("[[widescreen]]\n");
        assert!(g.is_empty());
    }

    #[test]
    fn out_of_range_width_is_discarded() {
        let g = WidescreenGate::from_toml("[[widescreen]]\nwidth = 999999\n");
        assert!(g.is_empty());
    }

    #[test]
    fn invalid_toml_yields_empty_gate() {
        let g = WidescreenGate::from_toml("[[widescreen\nwidth =");
        assert!(g.is_empty());
        assert_eq!(g.width_for(&FrameCtx::frame_only(0)), None);
    }
}
