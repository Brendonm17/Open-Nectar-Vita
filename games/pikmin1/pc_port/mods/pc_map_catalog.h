#ifndef _PC_MAP_CATALOG_H
#define _PC_MAP_CATALOG_H

#include "nectar_mapcore/map.h"

#include <string>
#include <vector>

/*
 * Mapas personalizados del usuario, de cualquier modo (VS, solo, coop):
 * user/maps/<id>/map.json y, por compatibilidad, user/maps/vs/<id>/map.json
 * (los de antes; su id sigue siendo el nombre de la carpeta, sin "vs/").
 * Si un id está en los dos sitios, gana el de user/maps/vs/.
 */
struct PcMapCatalogEntry {
	std::string id;     ///< nombre de la carpeta
	std::string file;   ///< ruta del map.json
	std::string name;   ///< del map.json (o el id si no se puede leer)
	std::string author;
	std::string thumb;  ///< ruta de thumb.png, vacía si no hay
	unsigned modes = 0; ///< nectar::MapMode; 0 si no se puede leer
	bool legacy    = false; ///< está en user/maps/vs/
	std::string error;  ///< por qué no se puede jugar (en ningún modo); vacío si vale
};

/// Todos los mapas del usuario por orden alfabético de id. Con rescan vuelve
/// a recorrer las carpetas.
const std::vector<PcMapCatalogEntry>& pc_map_catalog(bool rescan);

/// Ruta del map.json de un id (exista o no).
std::string pc_map_catalog_file(const std::string& id);

/// Lee y despliega un map.json. false con el motivo si no se puede.
bool pc_map_catalog_load(const std::string& path, nectar::MapData& out, std::string& error, std::string* source = nullptr);

#endif
