#pragma once

// Textos en español e inglés, el idioma que use el editor de mapas
// (config.json compartido, "language").

namespace mm {

void setSpanish(bool spanish);
bool spanish();
inline const char* L(const char* es, const char* en) { return spanish() ? es : en; }

} // namespace mm
