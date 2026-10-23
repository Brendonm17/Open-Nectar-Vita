#pragma once

// Campañas en paquete (docs/PLAN_CAMPANAS.md):
//   user/campaigns/<id>/campaign.json   reglas, zonas, puntos, desbloqueos, final
//   user/campaigns/<id>/zones/<zona>/map.json
// Como la historia del juego: días, Pikmin y cebollas son de la campaña; cada
// visita a una zona es un día y lo hecho en ella se queda así.
// No depende del juego.

#include "nectar_mapcore/map.h"

#include <map>
#include <string>
#include <vector>

namespace nectar {

/// Una condición para abrir una zona.
struct UnlockRule {
	/// "parts" (count piezas en total), "part" (la pieza id), "pikmin"
	/// (count Pikmin de color; color -1 = en total), "onion" (tener la
	/// cebolla color), "zone" (zona id completada), "visited" (zona id
	/// visitada), "day" (desde el día count) o "medal" (en la zona id, al
	/// menos la medalla count: 0 bronce, 1 plata, 2 oro).
	std::string type = "parts";
	int count = 1;
	std::string id;
	int color = -1;
};

/// Se abre si se cumplen todas las de `all` y, si hay alguna en `any`, al
/// menos una de ellas. Vacío = abierta desde el principio.
struct UnlockBlock {
	std::vector<UnlockRule> all, any;
	bool empty() const { return all.empty() && any.empty(); }
};

struct CampaignZone {
	std::string id, name;
	std::string nameEn; ///< en inglés (vacío = el mismo)
	double x = 0.5, y = 0.5; ///< punto en la imagen del mapa (0-1)
	std::string photo;       ///< "game:<snap>" o un PNG del paquete; vacío = la miniatura del nivel
	std::string music;       ///< vacío = la del nivel
	UnlockBlock unlock;
	/// Carpeta del nivel dentro del paquete.
	std::string folder() const { return "zones/" + id; }
};

struct CampaignStart {
	std::string zone;                    ///< vacío = la primera
	bool onions[3] = { false, true, false }; ///< azul, roja, amarilla
	int pikmin[3]  = { 0, 10, 0 };       ///< en el grupo, el primer día
};

struct CampaignEnding {
	/// "parts": count piezas (0 = todas las de la campaña); "zone": completar
	/// la zona `zone`.
	std::string type = "parts";
	int count = 0;
	std::string zone;
	int dayLimit = 0; ///< 0 = sin límite; si se pasa, final malo
	std::string goodText, badText;
	std::string goodTextEn, badTextEn;
	std::string image; ///< PNG del paquete para el final (vacío = sin)
};

/// Entrada del diario: se lee al anochecer (una vez), el día `day` (0 =
/// cualquiera) en la zona `zone` (vacío = cualquiera).
struct CampaignDiary {
	int day = 0;
	std::string zone, text, textEn;
};

/// Punto de interés del mapa de zonas: solo enseña su texto.
struct CampaignPoi {
	double x = 0.5, y = 0.5;
	std::string name, nameEn, text, textEn;
};

struct CampaignDoc {
	std::string name = "Campaña nueva", author, version = "1.0";
	std::string worldmap = "game"; ///< "game" (fondo del juego) o un PNG del paquete
	std::string intro;
	std::string nameEn, introEn;
	std::string introImage; ///< PNG del paquete para la introducción (vacío = sin)
	std::vector<CampaignDiary> diary;
	std::vector<CampaignPoi> pois;
	std::vector<CampaignZone> zones;
	std::vector<std::pair<std::string, std::string>> paths; ///< zonas unidas en el mapa
	CampaignStart start;
	CampaignEnding ending;

	/// Índice de la zona, o -1.
	int find(const std::string& id) const;
	/// La zona donde empieza (start.zone o la primera).
	int startZone() const;
};

bool parseCampaignDoc(const std::string& text, CampaignDoc& out, std::string& error);
std::string writeCampaignDoc(const CampaignDoc& doc);

/// Lo que se sabe de una partida para las reglas de desbloqueo y el final.
struct CampaignProgress {
	int day = 1;
	bool onions[3] = { false, false, false };
	int pikmin[3]  = { 0, 0, 0 };
	std::vector<std::string> parts;      ///< ids de las piezas recogidas
	std::vector<std::string> completed;  ///< zonas completadas
	std::vector<std::string> visited;
	std::map<std::string, int> medals;   ///< mejor medalla por zona
};

bool unlockMet(const UnlockBlock& block, const CampaignProgress& progress);
/// Abierta: la de salida siempre; las demás, por sus reglas.
bool zoneOpen(const CampaignDoc& doc, int zone, const CampaignProgress& progress);
/// Total de piezas de la campaña (de todos sus niveles).
int campaignPartTotal(const std::vector<const MapData*>& zoneMaps);
/// 1 = final bueno, 0 = final malo (se pasó de días), -1 = sigue.
int campaignEnding(const CampaignDoc& doc, const CampaignProgress& progress, int partTotal);

/// Comprueba el paquete con los mapas de sus zonas ya leídos (en el orden de
/// doc.zones; nullptr si ese no se pudo leer: el motivo ya lo dio quien lo
/// leyó). `coop` dice si todas las zonas valen también para dos jugadores.
bool validateCampaign(const CampaignDoc& doc, const std::vector<const MapData*>& zoneMaps, bool& coop, std::string& error);

} // namespace nectar
