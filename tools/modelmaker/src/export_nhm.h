#pragma once

// El pack que lee el juego (NHM1 versión 3, pc_port/mods/pc_hd_models.cpp):
// huesos con su inversa de reposo (con los pivotes propios), y hasta 8 piezas
// de triángulos sueltos con una textura RGBA cada una.

#include "import.h"
#include "rig.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mm {

struct NhmPart {
	std::vector<SkinVertex> verts; ///< joint[] = articulación de Olimar
	nectar::Image texture;
	unsigned flags = 0; ///< 1 repetir, 2 sin culling, 4 sin tinte
	std::vector<int> materials; ///< de dónde sale (para el informe)
};

/// Lo que dice el semáforo.
struct Check {
	enum Level { Ok, Warn, Bad } level = Ok;
	std::string text;
};

struct ExportResult {
	std::vector<NhmPart> parts;
	std::vector<Check> checks;
	std::size_t triangles = 0, bytes = 0;
	bool atlas = false;
	bool ok() const
	{
		for (const Check& c : checks)
			if (c.level == Check::Bad) return false;
		return !parts.empty();
	}
};

struct ExportOptions {
	int textureMax = 1024;
	bool pikmin    = false; ///< hasta 100 en pantalla: presupuesto de triángulos mucho menor
	std::vector<char> noTint; ///< por material: no lo tiñe el color del J2 en cooperativo
};

ExportResult buildParts(const ImpModel& model, const RigResult& rig, const OlimarRig& olimar, const FitSettings& fit, const ExportOptions& opt);

/// El .nhm en memoria (para un personaje .nectarchar) o en un archivo.
bool buildNhm(const ExportResult& result, const RigResult& rig, const OlimarRig& olimar, std::vector<std::uint8_t>& out, std::string& error);
bool writeNhm(const std::string& path, const ExportResult& result, const RigResult& rig, const OlimarRig& olimar, std::string& error);

} // namespace mm
