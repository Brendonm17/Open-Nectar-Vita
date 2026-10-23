#ifndef _PC_CAMPAIGN_H
#define _PC_CAMPAIGN_H

#include "types.h"
#include "nectar_mapcore/campaign.h"
#include "pc_mission.h"

#include <string>
#include <vector>

/*
 * Campañas en paquete (docs/PLAN_CAMPANAS.md): user/campaigns/<id>/ con su
 * campaign.json y zones/<zona>/map.json. Como la historia: días, Pikmin y
 * cebollas son de la partida; cada visita a una zona es un día (una misión
 * en «modo zona», mods/pc_mission). Tres ranuras por campaña en
 * user/saves/campaigns/<id>/slot1..3.txt.
 */

/// Una zona del paquete, leída al hacer la lista.
struct PcCampaignZoneInfo {
	std::string file;                ///< su map.json
	std::string thumb;               ///< thumb.png del nivel, vacío si no hay
	std::vector<std::string> parts;  ///< ids de sus piezas de la nave
	unsigned long long hash = 0;     ///< huella del map.json
	bool hasGoal = false;            ///< tiene objetivo (cumplirlo la completa)
};

struct PcCampaignPack {
	std::string id, dir;
	nectar::CampaignDoc doc;
	std::vector<PcCampaignZoneInfo> zones; ///< en el orden de doc.zones
	int partTotal = 0;
	bool coop     = false;           ///< todas las zonas valen para dos
	std::string thumb;
	std::string error;               ///< por qué no se puede jugar; vacío si vale
};

/// Todos los paquetes, por orden alfabético de id. Con rescan los vuelve a leer.
const std::vector<PcCampaignPack>& pc_campaign_packs(bool rescan);

// ── Ranuras ─────────────────────────────────────────────────────────────────

struct PcCampaignSlotInfo {
	bool used   = false;
	int players = 1, day = 0, parts = 0, pikmin = 0, zonesOpen = 0, zonesDone = 0;
	int ending  = -1; ///< 1 terminada, 0 final malo, -1 en curso
};
constexpr int PC_CAMPAIGN_SLOTS = 3;
PcCampaignSlotInfo pc_campaign_slot_info(const PcCampaignPack& pack, int slot);
void pc_campaign_slot_delete(const PcCampaignPack& pack, int slot);
/// Copia la ranura `from` en `to` (que se sobrescribe).
bool pc_campaign_slot_copy(const PcCampaignPack& pack, int from, int to);

// ── Partida en curso ────────────────────────────────────────────────────────

/// Abre una ranura: con `newPlayers` 1 o 2, una partida nueva con esos
/// modificadores (se guarda ya); con 0, la guardada. false si el paquete no
/// vale o la ranura está vacía.
bool pc_campaign_open(const PcCampaignPack& pack, int slot, int newPlayers, const PcMissionMods& mods = PcMissionMods());
/// Límite de días de la partida (el del paquete más los días extra; 0 = sin).
int pc_campaign_day_limit(void);
bool pc_campaign_active(void);
void pc_campaign_close(void);
const PcCampaignPack* pc_campaign_pack(void);
int pc_campaign_slot(void);
int pc_campaign_players(void);
int pc_campaign_day(void);
/// Zona donde está la nave (la última elegida).
int pc_campaign_current_zone(void);
int pc_campaign_ending(void); ///< como PcCampaignSlotInfo::ending
/// Lo que llevan las reglas (piezas, cebollas, Pikmin, zonas...).
const nectar::CampaignProgress& pc_campaign_progress(void);

bool pc_campaign_zone_open(int zone);
bool pc_campaign_zone_completed(int zone);
bool pc_campaign_zone_visited(int zone);
int pc_campaign_zone_medal(int zone);
/// Piezas de la zona ya recogidas.
int pc_campaign_zone_parts_found(int zone);
/// Qué falta para abrir la zona (la primera regla sin cumplir), para enseñarlo.
std::string pc_campaign_lock_text(int zone, bool spanish);

/// Prepara la misión de esa zona (mapa, lo hecho en ella, arena). false con
/// el motivo si no se puede jugar.
bool pc_campaign_enter(int zone, std::string& error);

// Lo que usa la misión en modo zona.
bool pc_campaign_has_onion(int color);
int pc_campaign_onion_pikmin(int color);
/// Los de esa cebolla por fase (hoja, capullo, flor).
void pc_campaign_onion_stages(int color, int out[3]);
/// Pikmin del principio en el grupo: solo la primera vez que se juega.
int pc_campaign_start_pikmin(int color);

/// Fin del día en la zona en curso.
struct PcCampaignDayEnd {
	std::string run;                    ///< pc_mission_run_text
	int onionPikmin[3] = { 0, 0, 0 };   ///< los que pasan la noche
	int onionStages[3][3] = {};         ///< por color y fase (hoja, capullo, flor)
	bool onions[3]     = { false, false, false }; ///< cebollas que había en la zona
	std::vector<std::string> parts;     ///< piezas de la zona ya entregadas
	bool completed = false;             ///< objetivo de la zona cumplido
	int medal      = -1;
};
/// Lo guarda, pasa al día siguiente y mira si se abren zonas o se acaba.
/// Devuelve el final (como pc_campaign_ending).
int pc_campaign_day_end(const PcCampaignDayEnd& info);
/// Entrada del diario leída en el último fin de día (vacía si no hubo).
const std::string& pc_campaign_diary(void);
/// Un texto del paquete en el idioma del juego (el inglés si lo hay y no se
/// juega en español).
const std::string& pc_campaign_text(const std::string& es, const std::string& en);
/// Piezas recogidas y las de toda la campaña (para la nave por fuera).
int pc_campaign_parts_count(void);
/// Zonas recién abiertas por el último fin de día (para avisar).
const std::vector<int>& pc_campaign_new_zones(void);
/// Ya se enseñaron.
void pc_campaign_clear_new_zones(void);

// ── Prueba desde el editor (--campaign <carpeta> [--zone <id>]) ─────────────

/// Pedida por la línea de órdenes: el juego va directo a esa zona de esa
/// campaña (partida de prueba: no se guarda en ninguna ranura).
/// `state`: estado de prueba, "day=5;pikmin=0 40 0;onions=1 1 0;parts=3"
/// (todo opcional; las piezas, las primeras de las zonas en orden).
void pc_campaign_direct_request(const std::string& dir, const std::string& zone, const std::string& state = std::string());
bool pc_campaign_direct_take_title(void); ///< en el título: true una vez
/// En CardSelect: true una vez; prepara la partida y la zona (o el motivo en
/// `error` si no se puede).
bool pc_campaign_direct_take_setup(std::string& error);
/// La zona ya está preparada: directo a jugarla, sin el mapa de zonas (una vez).
bool pc_campaign_take_direct_zone(void);

#endif
