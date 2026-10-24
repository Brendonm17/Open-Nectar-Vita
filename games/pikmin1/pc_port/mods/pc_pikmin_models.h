#ifndef PC_PIKMIN_MODELS_H
#define PC_PIKMIN_MODELS_H

/* Modelos de Pikmin propios (Load/Pikmin, del creador de modelos): uno por
   color, elegido en Opciones > Gráficos o en el launcher (se guarda por id en
   pikmin_settings.conf). `color` es el PikiColor del juego: 0 azul, 1 rojo,
   2 amarillo. Un pack "pikmin_all" vale para los tres y se tiñe del color. */

/// Vuelve a leer Load/Pikmin (al abrir los ajustes; antes, la primera vez).
void pc_pikmin_models_refresh(void);
/// Packs que sirven para ese color, por nombre.
int pc_pikmin_models_count(int color);
const char* pc_pikmin_models_id(int color, int index);
const char* pc_pikmin_models_name(int color, int index);
/// Nombre del pack con ese id ("" si no está instalado).
const char* pc_pikmin_models_name_of(int color, const char* id);
/// "red", "yellow", "blue" (o "" si no es un color).
const char* pc_pikmin_models_colour_key(int color);
int pc_pikmin_models_colour_from_key(const char* key);
/// El .nhm elegido para ese color (nullptr = el del juego), y si se tiñe.
const char* pc_pikmin_model_path(int color);
bool pc_pikmin_model_tinted(int color);

#endif
