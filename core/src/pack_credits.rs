//! Credits, licensing, and source provenance from `credits.toml`.
//!
//! The parser exposes attribution data without requiring consumers to interpret
//! pack metadata directly.

// Créditos y procedencia de un pack — `credits.toml`.
//
// Lo escribe el horneado del Lab a partir de la procedencia resuelta de cada
// asset, y existe para dos consumidores distintos:
//
//   · **Play**, que muestra la atribución del asset que está usando. Por eso
//     las entradas van por **id de contenido** y no por nombre de
//     archivo: es lo único que Play tiene en la mano al resolver una
//     sustitución, y además no regala el árbol del proyecto de origen.
//   · **Hub**, que muestra quién aportó qué. Por eso además hay `[[credit]]`
//     agrupado por persona: una lista por asset sería un inventario, no un
//     crédito.
//
// Se lee **a pedido** y no al abrir el pack: un pack sin créditos es válido, y
// parsearlo en `open` le pondría un costo a todos los que nunca lo consultan.

use serde::Deserialize;
use std::collections::HashMap;

/// A contributor and a summary of their contribution.
#[derive(Debug, Clone, Deserialize, Default)]
pub struct Credit {
    /// Contributor name.
    pub author: String,
    /// Optional role, such as artist or sound designer.
    #[serde(default)]
    pub role: Option<String>,
    /// Number of assets attributed to the contributor.
    #[serde(default)]
    pub assets: u32,
    /// Licenses associated with the contribution.
    #[serde(default)]
    pub licenses: Vec<String>,
}

/// Provenance and licensing metadata for one pack entry.
#[derive(Debug, Clone, Deserialize, Default)]
pub struct AssetCredit {
    /// Content identifier without the `assets/` prefix or tier.
    pub id: String,
    /// Optional contributor name.
    #[serde(default)]
    pub author: Option<String>,
    /// Optional role for this asset.
    #[serde(default)]
    pub role: Option<String>,
    /// Optional source URL or source description.
    #[serde(default)]
    pub source: Option<String>,
    /// Optional license identifier or name.
    #[serde(default)]
    pub license: Option<String>,
    /// Optional statement of the rights granted.
    #[serde(default)]
    pub rights: Option<String>,
    /// Optional permission record for non-standard licensing.
    #[serde(default)]
    pub permission: Option<String>,
    /// Preferred attribution text.
    #[serde(default)]
    pub attribution: Option<String>,
}

#[derive(Debug, Deserialize, Default)]
struct RawCredits {
    #[serde(default, rename = "credit")]
    credits: Vec<Credit>,
    #[serde(default, rename = "asset")]
    assets: Vec<AssetCredit>,
}

/// Parsed pack credits indexed by asset identifier.
#[derive(Debug, Default)]
pub struct Credits {
    /// Contributor summaries declared by the pack.
    pub credits: Vec<Credit>,
    /// Per-asset provenance declarations.
    pub assets: Vec<AssetCredit>,
    by_id: HashMap<String, usize>,
}

impl Credits {
    /// Parses `credits.toml`.
    ///
    /// Returns `None` for invalid UTF-8 or TOML. Credits are informational, so
    /// malformed metadata does not make the pack itself unplayable.
    pub fn parse(bytes: &[u8]) -> Option<Self> {
        let text = std::str::from_utf8(bytes).ok()?;
        let raw: RawCredits = toml::from_str(text).ok()?;
        let mut by_id = HashMap::new();
        for (i, a) in raw.assets.iter().enumerate() {
            by_id.insert(a.id.clone(), i);
        }
        Some(Credits {
            credits: raw.credits,
            assets: raw.assets,
            by_id,
        })
    }

    /// Looks up provenance metadata for an asset identifier.
    pub fn asset(&self, id: &str) -> Option<&AssetCredit> {
        self.by_id.get(id).map(|&i| &self.assets[i])
    }

    /// Returns the preferred display attribution for an asset.
    ///
    /// Falls back to the author when no explicit attribution is provided.
    pub fn attribution_of(&self, id: &str) -> Option<&str> {
        let a = self.asset(id)?;
        a.attribution
            .as_deref()
            .filter(|s| !s.is_empty())
            .or(a.author.as_deref().filter(|s| !s.is_empty()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"
[[credit]]
author = "Ana Ruiz"
role = "colaboradora"
assets = 2
licenses = ["CC-BY-4.0", "CC0-1.0"]

[[credit]]
author = "David"
assets = 1
licenses = ["CC-BY-NC-4.0"]

[[asset]]
id = "aaaa"
author = "Ana Ruiz"
license = "CC-BY-4.0"
attribution = "Ana Ruiz, CC BY 4.0"

[[asset]]
id = "bbbb"
author = "David"
license = "CC-BY-NC-4.0"
"#;

    #[test]
    fn lee_creditos_y_procedencia() {
        let c = Credits::parse(SAMPLE.as_bytes()).unwrap();
        assert_eq!(c.credits.len(), 2);
        assert_eq!(c.credits[0].author, "Ana Ruiz");
        assert_eq!(c.credits[0].role.as_deref(), Some("colaboradora"));
        assert_eq!(c.credits[0].licenses.len(), 2);
        assert_eq!(
            c.asset("aaaa").unwrap().license.as_deref(),
            Some("CC-BY-4.0")
        );
    }

    #[test]
    fn la_atribucion_cae_al_autor_cuando_no_hay_texto() {
        let c = Credits::parse(SAMPLE.as_bytes()).unwrap();
        assert_eq!(c.attribution_of("aaaa"), Some("Ana Ruiz, CC BY 4.0"));
        // Sin `attribution`: decir quién lo hizo sigue siendo mejor que nada.
        assert_eq!(c.attribution_of("bbbb"), Some("David"));
        assert_eq!(c.attribution_of("no-existe"), None);
    }

    /// Un `credits.toml` roto NO puede tirar el pack: los créditos son
    /// informativos y el juego tiene que seguir andando. Lo que no puede es
    /// inventar una atribución, y `None` es exactamente eso.
    #[test]
    fn un_archivo_roto_da_none_y_no_un_panic() {
        assert!(Credits::parse(b"[[credit] esto no es toml").is_none());
        assert!(Credits::parse(&[0xFF, 0xFE, 0x00]).is_none());
        // Vacío es válido y no es lo mismo que roto: un pack puede traer el
        // archivo sin nada adentro.
        let empty = Credits::parse(b"").unwrap();
        assert!(empty.credits.is_empty() && empty.assets.is_empty());
    }
}
