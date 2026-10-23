#ifndef _PC_MISSION_H
#define _PC_MISSION_H

#include "types.h"
#include "nectar_mapcore/map.h"

#include <string>
#include <vector>

/*
 * Misiones: mapas personalizados en modo solo o coop (sección "story" del
 * map.json). Van por el camino del modo desafío, como el VS, con la arena de
 * mods/pc_vs_arena; aquí, qué mapa se juega, el objetivo, los días y su
 * guardado (user/saves/maps/<id>/solo.txt o coop.txt).
 *
 * Una zona de una campaña (mods/pc_campaign) también es una misión, en «modo
 * zona»: un día por visita, Pikmin y cebollas de la campaña, y lo hecho en la
 * zona lo guarda la campaña (pc_mission_run_text).
 */

/// Elegido en el título ("Custom Maps"). Lo apaga cualquier otra opción.
void pc_mission_set_pending(bool on);
bool pc_mission_pending(void);
/// GameCoreSection, junto a pc_coop_begin_run / pc_coop_end_run.
void pc_mission_begin_run(void);
bool pc_mission_active(void);
void pc_mission_end_run(void);

/// Mapa de la misión (id del catálogo, pc_map_catalog) y jugadores: 1 = solo,
/// 2 = coop (el mapa tiene que valer para ese modo).
void pc_mission_select(const std::string& id, int players);
const std::string& pc_mission_selected(void);
int pc_mission_players(void);
/// Un map.json cualquiera por su ruta (opción --mission-map del editor). Sin
/// guardado: cada prueba empieza en el día 1.
void pc_mission_select_file(const std::string& path, int players);
/// Una zona de la campaña en curso: su map.json y lo que ya se hizo en ella
/// (pc_mission_run_text de la última visita; vacío = la primera).
void pc_mission_select_zone(const std::string& path, int players, const std::string& runText);
/// La misión es una zona de campaña.
bool pc_mission_zone_mode(void);

/// --mission-map: el juego va directo a la misión, sin título ni menús (J1
/// teclado + mando 1; en coop, J2 mando 2).
void pc_mission_direct_request(void);
bool pc_mission_direct_take_title(void); ///< en el título: true una vez
bool pc_mission_direct_take_setup(void); ///< en CardSelect: true una vez
/// Cambia cada vez que se elige otro mapa (para tirar lo compilado).
int pc_mission_serial(void);
/// El mapa elegido, ya leído; nullptr si no vale para el modo (el motivo, en
/// pc_mission_error).
const nectar::MapData* pc_mission_map(void);
const std::string& pc_mission_error(void);
/// Huella del map.json elegido (para saber si un guardado es de otra versión).
unsigned long long pc_mission_map_hash(void);
/// Lo hecho en el mapa (piezas, pastillas, jefes, construcciones,
/// disparadores), en una línea.
std::string pc_mission_run_text(void);

// ── Modificadores, música y horas ───────────────────────────────────────────

/// Se eligen al empezar una misión o una partida de campaña (y se guardan con
/// ella). Con alguno distinto del normal, los récords van aparte.
struct PcMissionMods {
	int tekiHealth = 100;  ///< vida de los enemigos, en %
	int extraDays  = 0;    ///< días de más (misiones de varios días, límite de la campaña)
	bool noLosses  = false; ///< misión suelta: se pierde si cae un Pikmin
	int carry      = 100;  ///< velocidad de carga, en %
	bool any() const { return tekiHealth != 100 || extraDays || noLosses || carry != 100; }
};
void pc_mission_set_mods(const PcMissionMods& mods);
const PcMissionMods& pc_mission_mods(void);
/// Música de la misión: índice en nectar::musicNames() (la de la zona de la
/// campaña o la del mapa), o -1 = la de siempre.
int pc_mission_music(void);
/// Horas del día de la misión (las del mapa o `fallback`).
f32 pc_mission_start_hour(f32 fallback);
f32 pc_mission_end_hour(f32 fallback);

// ── Guardado ────────────────────────────────────────────────────────────────

/// Día guardado del mapa elegido (para esos jugadores), o 0 si no hay o es
/// de otra versión del mapa.
int pc_mission_saved_day(void);
/// Empieza la misión: desde el día 1 o desde lo guardado (si lo hay).
void pc_mission_start(bool continueSaved);

// ── Partida ─────────────────────────────────────────────────────────────────

/// Día en curso (desde 1) y días de la misión.
int pc_mission_day(void);
int pc_mission_days(void);
/// Al montar el mapa (initStage): empieza el día en curso.
void pc_mission_begin_day(void);

/// Lo que ya no vuelve a salir (índices en las listas de MapData).
bool pc_mission_part_done(int index);
bool pc_mission_pellet_done(int index);
bool pc_mission_object_done(int objectIndex);
/// Si el enemigo sale hoy: sus días (fromDay/untilDay) y, si se le derrotó,
/// su regla de reaparición (por defecto, los jefes no vuelven y los demás sí).
bool pc_mission_enemy_spawns(int enemyIndex);
/// Estado guardado de una construcción (Creature::doSave, en hex; vacío si no
/// hay): puentes a medias, rocas empujadas, compuertas tocadas.
const std::string& pc_mission_object_state(int objectIndex);
/// Caché de generadores del juego (GeneratorCache::saveCard, en hex; vacía si
/// no hay): lo que hicieron los generadores del escenario los días anteriores.
const std::string& pc_mission_gen_cache();

/// Cebollas que se ponen al montar el mapa: color, sitio y los Pikmin que
/// guarda. En una zona de campaña, las de la campaña (en su sitio del mapa o
/// junto a la nave) y las del mapa que aún no se tienen (se encuentran).
struct PcMissionOnion {
	int color;
	f32 x, z;
	int stages[3]; ///< Pikmin que guarda: hoja, capullo, flor
	bool dormant;  ///< enterrada: hay que encontrarla (pc_mission_note_onion_found)
};
std::vector<PcMissionOnion> pc_mission_onions(void);
/// Nivel de la nave por fuera (0-5) según las piezas recogidas.
int pc_mission_ship_level(void);
/// Pikmin de ese color en el grupo al empezar (solo el primer día).
int pc_mission_start_pikmin(int color);

/// Pieza de la nave entregada / pastilla llevada a una cebolla (solo cuentan
/// las del mapa, no los enemigos).
void pc_mission_note_part(u32 pelletId);
void pc_mission_note_pellet(u32 pelletId);
/// Un capitán ha despertado una cebolla enterrada.
void pc_mission_note_onion_found(int color);
/// Cada fotograma: semillas salidas de las cebollas y enemigos derrotados hoy
/// (GameStat), para los resultados.
void pc_mission_note_counts(int born, int kills);
/// Lo que miran las condiciones de los disparadores, cada fotograma.
struct PcMissionWorld {
	f32 hour        = 0.0f;
	int pikmin[3]   = { 0, 0, 0 }; ///< por color (campo, brotes y cebollas)
	int captains    = 0;
	f32 cx[2]       = { 0, 0 }, cz[2] = { 0, 0 };
	int waveAlive   = 0; ///< enemigos de oleadas aún vivos
};
/// Cada fotograma: Pikmin que quedan (campo, brotes y cebollas), jefes vivos
/// y lo demás que miran los disparadores.
void pc_mission_update(f32 dt, int pikmin, int bossesAlive, const PcMissionWorld& world);
/// Construcción terminada o rota (índice en la lista desplegada), cada fotograma.
void pc_mission_note_object_done(int objectIndex);
/// Acción de un disparador que tiene que hacer el juego (aparecer, romper):
/// true y cuál (disparador y acción en MapStory::triggers). Mensajes, cuadros,
/// cuentas atrás, ganar y perder los lleva la misión.
bool pc_mission_take_action(int& trigger, int& action, bool& wave);
/// Hay que acabar el día ya (acción, cuenta atrás o límite de tiempo).
bool pc_mission_take_end_day(void);
/// Mensaje de un disparador (o de la zona completada) mientras dura.
const char* pc_mission_message(void);
/// Cuadro de texto abierto (el juego se para hasta que se cierra con A).
const char* pc_mission_textbox(void);
void pc_mission_close_textbox(void);
/// Acción «cámara»: segundos que la cámara sigue mirando otra cosa (el juego
/// la apunta y la devuelve); mientras, nadie se mueve.
void pc_mission_set_camera_hold(f32 seconds);
f32 pc_mission_camera_hold(void);
/// Cuenta atrás en marcha: segundos que quedan (< 0 si no hay).
f32 pc_mission_countdown(void);

/// Fin del día (GameCoreSection, al llegar la hora): primero lo que ya no
/// vuelve (jefes, construcciones), luego los Pikmin que pasan la noche en
/// cada cebolla y los que se pierden. Decide el resultado; si quedan días y
/// no se ha decidido nada, guarda y pasa al siguiente. En una zona de
/// campaña, se lo pasa a la campaña.
void pc_mission_set_enemy_dead(int enemyIndex);
void pc_mission_set_object_done(int objectIndex);
void pc_mission_set_object_state(int objectIndex, const std::string& hex);
void pc_mission_set_gen_cache(const std::string& hex);
/// `night`: los que pasan la noche por color (azul, rojo, amarillo) y fase
/// (hoja, capullo, flor).
void pc_mission_end_day(const int night[3][3], int lost);
/// -1 en juego, 0 perdida, 1 cumplida, 2 fin de un día (sigue otro).
int pc_mission_result(void);
bool pc_mission_over(void);
/// Salidas al acabar: 1 = al título, 2 = al día siguiente, 3 = al mapa de
/// zonas de la campaña, 4 = reintentar desde el principio.
enum { PC_MISSION_EXIT_TITLE = 1, PC_MISSION_EXIT_NEXT_DAY, PC_MISSION_EXIT_MAP, PC_MISSION_EXIT_RETRY };
/// La pantalla de resultados está a la vista (un poco después de acabar).
bool pc_mission_end_screen(void);
/// Las salidas que ofrece ahora (la primera es la de por defecto).
int pc_mission_end_options(int* kinds, int max);
/// Elegida en la pantalla de resultados.
void pc_mission_choose_exit(int kind);
/// La salida elegida (una sola vez); 0 mientras no hay.
int pc_mission_take_exit(void);
/// Reintentar: la misma misión desde el principio (y los mismos modificadores).
void pc_mission_retry(void);

/// Lo que enseña la pantalla de resultados.
struct PcMissionStats {
	f32 seconds = 0;          ///< tiempo de juego del día
	int day = 1, days = 1;    ///< días = 0 en una zona de campaña
	int born = 0, lost = 0, kills = 0, pikmin = 0;
	int night[3] = { 0, 0, 0 }; ///< los que pasaron la noche, por color
	int lostAtNight = 0;
	int medal = -1;
	bool newRecord = false;
};
PcMissionStats pc_mission_stats(void);

// ── Récords y medallas ──────────────────────────────────────────────────────

struct PcMissionRecord {
	int wins   = 0;
	int medal  = -1; ///< la mejor: -1 ninguna, 0 bronce, 1 plata, 2 oro
	int pikmin = 0;  ///< los más Pikmin al cumplirla
	int days   = 0;  ///< los menos días (0 = nunca cumplida)
};
/// Récord de un mapa del catálogo para esos jugadores.
PcMissionRecord pc_mission_record(const std::string& id, int players);
/// "Bronce", "Plata", "Oro" (o "").
const char* pc_mission_medal_name(int medal, bool spanish);

/// Progreso para el HUD ("Día 2/3  Piezas 2/5"...).
void pc_mission_status(char* out, int n, bool spanish);
/// Aviso grande en pantalla (vacío si no hay).
const char* pc_mission_announcement(bool spanish);

#endif
