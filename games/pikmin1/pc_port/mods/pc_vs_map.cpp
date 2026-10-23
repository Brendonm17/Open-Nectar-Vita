#include "pc_vs_map.h"
#include "pc_map_catalog.h"

#include "pc_vs_default_map.h" // generado por CMake desde maps/vs/impact_arena/map.json

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* kDiskMap  = "maps/vs/impact_arena/map.json";

std::string sSelected; // "" = la arena de serie
std::string sFile;     // si no está vacío, un map.json por ruta (--vs-map)
int sSerial = 0;
PcVsMap sMap;
int sState = 0; // 0 sin leer, 1 listo, -1 error
std::string sError;
std::string sText; // el map.json del mapa activo
std::vector<PcVsMapEntry> sList;

bool readText(const std::string& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}


// La arena de serie: la de disco si vale, si no la incluida.
bool loadBuiltIn(PcVsMap& out, std::string& error, std::string* source = nullptr)
{
	std::string text;
	if (readText(kDiskMap, text)) {
		if (nectar::parseMap(text, out, error)) {
			fprintf(stderr, "[VS map] %s: \"%s\"\n", kDiskMap, out.name.c_str());
			if (source) *source = text;
			return true;
		}
		fprintf(stderr, "[VS map] %s no es válido (%s); se usa el incluido\n", kDiskMap, error.c_str());
	}
	if (source) *source = kPcVsDefaultMap;
	return nectar::parseMap(kPcVsDefaultMap, out, error);
}

// Un mapa de usuario: tiene que poder jugarse en VS.
bool loadPath(const std::string& path, PcVsMap& out, std::string& error, std::string* source = nullptr)
{
	if (!pc_map_catalog_load(path, out, error, source)) return false;
	if (!(out.modes & nectar::MAPMODE_VS)) {
		error = "este mapa no es para VS";
		return false;
	}
	return true;
}

} // namespace

const std::vector<PcVsMapEntry>& pc_vs_map_list(bool rescan)
{
	if (!sList.empty() && !rescan) return sList;
	sList.clear();

	PcVsMapEntry builtIn;
	PcVsMap m;
	if (loadBuiltIn(m, builtIn.error)) {
		builtIn.name   = m.name;
		builtIn.author = m.author;
	} else {
		builtIn.name = "Impact Arena";
	}
	sList.push_back(builtIn);

	// Los de VS del catálogo y los que no se pueden leer de la carpeta de VS
	// (para enseñar el motivo).
	for (const PcMapCatalogEntry& c : pc_map_catalog(rescan)) {
		if (!(c.modes & nectar::MAPMODE_VS) && !(c.modes == 0 && c.legacy)) continue;
		sList.push_back({ c.id, c.name, c.author, c.thumb, c.error });
	}
	return sList;
}

void pc_vs_map_select(const std::string& id)
{
	if (id == sSelected && sFile.empty() && sState > 0) return;
	sSelected = id;
	sFile.clear();
	sState = 0;
	sSerial++;
}

void pc_vs_map_select_file(const std::string& path)
{
	sSelected.clear();
	sFile  = path;
	sState = 0;
	sSerial++;
}

const std::string& pc_vs_map_selected() { return sSelected; }
int pc_vs_map_serial() { return sSerial; }
const std::string& pc_vs_map_error() { return sError; }
const std::string& pc_vs_map_source_text() { return sText; }

const PcVsMap* pc_vs_map()
{
	if (sState) return sState > 0 ? &sMap : nullptr;
	sError.clear();
	sText.clear();
	bool ok;
	if (!sFile.empty()) ok = loadPath(sFile, sMap, sError, &sText);
	else if (sSelected.empty()) ok = loadBuiltIn(sMap, sError, &sText);
	else ok = loadPath(pc_map_catalog_file(sSelected), sMap, sError, &sText);
	if (!ok) sText.clear();
	if (!ok) {
		const std::string which = !sFile.empty() ? sFile : (sSelected.empty() ? "arena de serie" : sSelected);
		fprintf(stderr, "[VS map] %s: %s\n", which.c_str(), sError.c_str());
	}
	sState = ok ? 1 : -1;
	return ok ? &sMap : nullptr;
}
