#pragma once
// ---------------------------------------------------------------------------
// failure_escalation.h — cuándo dejar de intentar ().
//
// El fallback de  ya evita que un asset roto corte la sesión: se oye el
// original y listo. Lo que falta es la ESCALADA — un pack con muchos assets
// rotos reintenta cada uno, cada frame, y paga la resolución completa por algo
// que ya se sabe que no va a andar. Ése es el riesgo que  anota.
//
// LA REGLA, que es lo único que hay acá:
//
//   Se cuentan ASSETS DISTINTOS, no ocurrencias.
//
// Un archivo roto que suena mil veces es UN problema; doce archivos distintos
// es un pack mal armado o una carpeta que no llegó. Contar ocurrencias apagaría
// el subsistema por un solo asset que se repite mucho — que es justo el caso
// que NO hay que castigar, porque el fallback ya lo resuelve bien.
//
// Y se cuenta POR SUBSISTEMA: que falte la música no dice nada sobre los
// efectos, y apagar los dos por uno sería llevarse puesto lo que sí funciona.
//
// Header-only y sin dependencias: se testea sin sesión, sin audio y sin ROM.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ayther {

class FailureEscalation {
public:
    /// Doce archivos distintos. Un par de assets rotos no es un desastre y el
    /// fallback los cubre sin que se note; una docena ya es un pack que salió
    /// mal, y seguir intentándolos cuesta más de lo que rinde.
    static constexpr size_t kDefaultThreshold = 12;

    explicit FailureEscalation(size_t threshold = kDefaultThreshold)
        : threshold_(threshold ? threshold : 1) {}

    /// Registra que `asset` falló en `subsystem`.
    ///
    /// Devuelve true SÓLO en el fallo que cruza el umbral — una vez, no en cada
    /// llamada posterior. Si devolviera true siempre, el llamador apagaría un
    /// subsistema ya apagado en cada frame y el log crecería sin decir nada
    /// nuevo.
    bool note(uint32_t subsystem, const std::string& asset) {
        if (asset.empty()) return false;
        auto& seen = failed_[subsystem];
        if (!seen.insert(asset).second) return false;   // ya contado
        return seen.size() == threshold_;
    }

    /// Cuántos assets distintos fallaron en ese subsistema.
    size_t count(uint32_t subsystem) const {
        const auto it = failed_.find(subsystem);
        return it == failed_.end() ? 0 : it->second.size();
    }

    /// El total, para el mensaje al usuario.
    size_t total() const {
        size_t n = 0;
        for (const auto& [s, set] : failed_) { (void)s; n += set.size(); }
        return n;
    }

    size_t threshold() const { return threshold_; }
    void   clear() { failed_.clear(); }

private:
    size_t threshold_;
    std::unordered_map<uint32_t, std::unordered_set<std::string>> failed_;
};

}  // namespace ayther
