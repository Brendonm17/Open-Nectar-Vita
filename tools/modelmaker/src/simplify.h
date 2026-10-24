#pragma once

// Menos triángulos sin estropear las texturas: colapso de aristas con
// cuádricas (Garland-Heckbert) quedándose siempre con uno de los dos
// vértices, así sus UV, normal y pesos siguen siendo los suyos. Las costuras
// de UV, los cambios de material y los bordes no se tocan.

#include "import.h"

namespace mm {

/// Deja el modelo en unos `targetTris` triángulos (o los que se pueda sin
/// romper costuras). Devuelve cuántos quedan.
std::size_t simplifyModel(ImpModel& model, std::size_t targetTris);

} // namespace mm
