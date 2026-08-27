//! Brightness-independent shape identities for VDP tiles.
//!
//! Relative palette-index ordering separates geometry from fade level, allowing
//! visually identical fade steps to share one authored asset family.

// shape_hash.rs — familias de tiles por FORMA, con el brillo aparte.
//
// # El problema, medido en cualquier juego
//
// Los juegos retro hacen fade-in y fade-out todo el tiempo, y en la Mega Drive
// eso se hace de dos maneras distintas:
//
// 1. **Cambiando la paleta** (CRAM). El contenido del tile no se toca, así que
//    el hash de sprite —que es ciego a paleta— ya agrupa esas variantes solo.
//    No hace falta nada nuevo.
// 2. **Cambiando el contenido**: el juego escribe tiles con índices más
//    oscuros. Ahí cada paso del fade es un tile distinto, con su propio hash, y
//    un autor que quiera sustituirlos tiene que dibujar el mismo asset ocho
//    veces.
//
// Esto ataca el caso 2, que es el que cuesta autoría.
//
// # Cómo se separa la forma del brillo
//
// Un fade por contenido cambia los VALORES de los índices manteniendo su
// disposición: donde había 6 ahora hay 4, donde había 4 ahora hay 2. La forma
// —qué píxel es más claro que cuál— no se mueve.
//
// Así que el shape-hash no hashea los índices sino su **orden relativo**: cada
// índice se reemplaza por su posición en la lista ordenada de los índices
// distintos que aparecen en ese tile. `{2,4,6}` y `{1,2,3}` dan los dos
// `{1,2,3}`, y el hash coincide.
//
// # El índice 0 NO participa
//
// En la Mega Drive el 0 es transparente. Se deja como 0 y el ranking empieza en
// 1, así que la SILUETA entra al hash: un tile con un agujero y otro sin él no
// son la misma forma por más que sus colores rankeen igual. Sin esta
// separación, un fade a negro terminaría agrupado con el tile vacío.

use xxhash_rust::xxh3::xxh3_64;

/// Bytes in one 8×8, 4-bpp Mega Drive tile.
pub const TILE_BYTES: usize = 32;
/// Pixels in one Mega Drive tile.
pub const TILE_PIXELS: usize = 64;

/// Decodes the 64 color indices of a tile, high nibble first.
pub fn tile_indices(tile: &[u8]) -> [u8; TILE_PIXELS] {
    let mut out = [0u8; TILE_PIXELS];
    for (i, o) in out.iter_mut().enumerate() {
        let b = tile.get(i / 2).copied().unwrap_or(0);
        *o = if i % 2 == 0 {
            (b >> 4) & 0x0F
        } else {
            b & 0x0F
        };
    }
    out
}

/// Computes a brightness-invariant, silhouette-sensitive shape hash.
///
/// Tiles with the same relative arrangement of light and dark indices share a
/// value even when the absolute indices differ. This complements, rather than
/// replaces, the content identity hash.
pub fn shape_hash(tile: &[u8]) -> u64 {
    let idx = tile_indices(tile);

    // Los índices distintos NO transparentes, ordenados. Son 15 como mucho, así
    // que un array chico gana a cualquier estructura: esto corre por tile.
    let mut presentes = [false; 16];
    for &v in idx.iter() {
        presentes[v as usize] = true;
    }
    let mut rango = [0u8; 16];
    let mut siguiente = 1u8;
    for v in 1..16usize {
        // el 0 se queda en 0: la silueta entra al hash
        if presentes[v] {
            rango[v] = siguiente;
            siguiente += 1;
        }
    }

    let normalizado: Vec<u8> = idx.iter().map(|&v| rango[v as usize]).collect();
    xxh3_64(&normalizado)
}

/// Returns the mean non-transparent palette index in the range `0..=15`.
///
/// A fully transparent tile returns `None`.
pub fn mean_level(tile: &[u8]) -> Option<f32> {
    let idx = tile_indices(tile);
    let (suma, n) = idx
        .iter()
        .filter(|&&v| v != 0)
        .fold((0u32, 0u32), |(s, n), &v| (s + v as u32, n + 1));
    if n == 0 {
        None
    } else {
        Some(suma as f32 / n as f32)
    }
}

/// Computes the brightness of `tile` relative to `referencia`.
///
/// `1.0` means equal brightness and a fade to black approaches zero. Returns
/// `None` when either input has no opaque pixels.
pub fn brightness_factor(tile: &[u8], referencia: &[u8]) -> Option<f32> {
    let (a, r) = (mean_level(tile)?, mean_level(referencia)?);
    if r <= 0.0 {
        return None;
    }
    Some(a / r)
}

/// Tiles that share a shape, together with their relative brightness factors.
#[derive(Debug, Clone, PartialEq)]
pub struct Family {
    /// Shape-only identity shared by every member.
    pub shape: u64,
    /// Input indices ordered from brightest to darkest.
    ///
    /// The first member is the authoring reference because it retains the most
    /// visual information.
    pub members: Vec<usize>,
    /// Brightness factor for each member; the first is always `1.0`.
    pub factors: Vec<f32>,
}

/// Groups tiles into shape families in deterministic first-seen order.
pub fn group_by_shape(tiles: &[&[u8]]) -> Vec<Family> {
    let mut fams: Vec<Family> = Vec::new();
    for (i, t) in tiles.iter().enumerate() {
        // Un tile todo transparente no forma familia: no tiene forma que
        // comparar ni brillo que medir, y agruparlo arrastraría a cualquier
        // otro que también esté vacío como si fueran variantes del mismo
        // dibujo.
        if mean_level(t).is_none() {
            continue;
        }
        let h = shape_hash(t);
        match fams.iter_mut().find(|f| f.shape == h) {
            Some(f) => f.members.push(i),
            None => fams.push(Family {
                shape: h,
                members: vec![i],
                factors: Vec::new(),
            }),
        }
    }
    // La referencia de cada familia es el miembro MÁS CLARO, y los factores se
    // calculan contra él. Se hace acá y no al insertar porque el más claro
    // puede aparecer último.
    for f in fams.iter_mut() {
        f.members.sort_by(|&a, &b| {
            let (la, lb) = (
                mean_level(tiles[a]).unwrap_or(0.0),
                mean_level(tiles[b]).unwrap_or(0.0),
            );
            lb.partial_cmp(&la)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then(a.cmp(&b)) // desempate estable por índice
        });
        let refer = tiles[f.members[0]];
        f.factors = f
            .members
            .iter()
            .map(|&m| brightness_factor(tiles[m], refer).unwrap_or(1.0))
            .collect();
    }
    fams
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Un tile con un patrón conocido: una diagonal de nivel `alto` sobre fondo
    /// de nivel `bajo`, con una esquina transparente.
    fn tile(alto: u8, bajo: u8) -> Vec<u8> {
        let mut px = [0u8; TILE_PIXELS];
        for y in 0..8usize {
            for x in 0..8usize {
                px[y * 8 + x] = if x == y { alto } else { bajo };
            }
        }
        px[0] = 0; // la esquina transparente: define la SILUETA
        let mut t = vec![0u8; TILE_BYTES];
        for i in 0..TILE_PIXELS {
            if i % 2 == 0 {
                t[i / 2] |= (px[i] & 0xF) << 4;
            } else {
                t[i / 2] |= px[i] & 0xF;
            }
        }
        t
    }

    /// An eight-step synthetic fade forms one family with eight brightness factors.
    #[test]
    fn un_fade_de_ocho_pasos_es_una_familia() {
        // Ocho pasos: el par (alto, bajo) baja parejo, que es lo que hace un
        // fade por contenido.
        let pasos: Vec<Vec<u8>> = (0..8).map(|i| tile(15 - i, 8 - i / 2)).collect();
        let refs: Vec<&[u8]> = pasos.iter().map(|v| v.as_slice()).collect();
        let fams = group_by_shape(&refs);

        assert_eq!(fams.len(), 1, "los ocho pasos son la MISMA forma");
        assert_eq!(fams[0].members.len(), 8);
        assert_eq!(fams[0].factors.len(), 8);
        assert!(
            (fams[0].factors[0] - 1.0).abs() < 1e-6,
            "la referencia es 1.0"
        );
        // Y los factores BAJAN: si fueran todos 1.0 el agrupamiento no
        // aportaría nada, porque no habría cómo reproducir cada paso.
        assert!(fams[0].factors[7] < fams[0].factors[0]);
        assert!(
            fams[0].factors.windows(2).all(|w| w[1] <= w[0] + 1e-6),
            "monotonos: el fade va en un solo sentido"
        );
    }

    /// La otra mitad del criterio: dos formas distintas NO se agrupan.
    #[test]
    fn dos_formas_distintas_no_se_agrupan() {
        let diagonal = tile(15, 8);
        // Misma paleta de niveles, disposición distinta: barras horizontales.
        let mut px = [0u8; TILE_PIXELS];
        for y in 0..8usize {
            for x in 0..8usize {
                px[y * 8 + x] = if y % 2 == 0 { 15 } else { 8 };
            }
        }
        px[0] = 0;
        let mut barras = vec![0u8; TILE_BYTES];
        for i in 0..TILE_PIXELS {
            if i % 2 == 0 {
                barras[i / 2] |= (px[i] & 0xF) << 4;
            } else {
                barras[i / 2] |= px[i] & 0xF;
            }
        }
        assert_ne!(shape_hash(&diagonal), shape_hash(&barras));
        let refs: Vec<&[u8]> = vec![&diagonal, &barras];
        assert_eq!(group_by_shape(&refs).len(), 2);
    }

    /// La SILUETA entra al hash. Sin esto, un fade a negro terminaría agrupado
    /// con el tile vacío — y peor, dos dibujos con distinto contorno se
    /// tratarían como el mismo si sus colores rankean igual.
    #[test]
    fn la_silueta_cuenta() {
        let con_hueco = tile(15, 8); // px[0] = 0
        let mut sin_hueco = con_hueco.clone();
        sin_hueco[0] = (8 << 4) | (sin_hueco[0] & 0x0F); // tapar el transparente
        assert_ne!(
            shape_hash(&con_hueco),
            shape_hash(&sin_hueco),
            "tapar el transparente cambia la forma"
        );
    }

    /// El shape-hash NO reemplaza al de identidad: dos tiles de la misma
    /// familia son tiles DISTINTOS, y el motor los tiene que seguir
    /// distinguiendo.
    #[test]
    fn la_familia_no_borra_la_identidad() {
        let claro = tile(15, 8);
        let oscuro = tile(7, 4);
        assert_eq!(shape_hash(&claro), shape_hash(&oscuro), "misma familia");
        assert_ne!(claro, oscuro, "y sin embargo son tiles distintos");
        assert!(mean_level(&claro).unwrap() > mean_level(&oscuro).unwrap());
    }

    /// La referencia es el más CLARO, aunque aparezca último. Un asset hecho
    /// sobre el paso más oscuro y aclarado después inventa detalle que no
    /// estaba.
    #[test]
    fn la_referencia_es_el_mas_claro() {
        let oscuro = tile(6, 3);
        let claro = tile(15, 8);
        let refs: Vec<&[u8]> = vec![&oscuro, &claro]; // el claro va segundo
        let fams = group_by_shape(&refs);
        assert_eq!(fams.len(), 1);
        assert_eq!(fams[0].members[0], 1, "el mas claro primero");
        assert!((fams[0].factors[0] - 1.0).abs() < 1e-6);
        assert!(fams[0].factors[1] < 1.0, "el oscuro se reproduce atenuando");
    }

    /// Un tile todo transparente no forma familia: no tiene forma que comparar
    /// ni brillo que medir, y agruparlo arrastraría a cualquier otro vacío como
    /// si fueran variantes del mismo dibujo.
    #[test]
    fn un_tile_vacio_no_forma_familia() {
        let vacio = vec![0u8; TILE_BYTES];
        assert_eq!(mean_level(&vacio), None);
        let otro_vacio = vec![0u8; TILE_BYTES];
        let refs: Vec<&[u8]> = vec![&vacio, &otro_vacio];
        assert!(group_by_shape(&refs).is_empty());
    }

    /// NO VACUIDAD del fixture: los ocho pasos del fade son tiles REALMENTE
    /// distintos. Si `tile()` devolviera lo mismo para todos, el test de la
    /// familia pasaría sin probar nada.
    #[test]
    fn los_pasos_del_fade_son_distintos_de_verdad() {
        let pasos: Vec<Vec<u8>> = (0..8).map(|i| tile(15 - i, 8 - i / 2)).collect();
        for i in 1..8 {
            assert_ne!(pasos[i], pasos[i - 1], "el paso {i} es igual al anterior");
        }
        // Y sus niveles medios bajan de verdad.
        let niveles: Vec<f32> = pasos.iter().map(|t| mean_level(t).unwrap()).collect();
        assert!(niveles[7] < niveles[0]);
    }
}
