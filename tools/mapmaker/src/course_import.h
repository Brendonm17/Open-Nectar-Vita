#pragma once

// Escenarios del juego como base de un mapa VS: el .mod del nivel tal cual
// y sus generadores (enemigos, plantas, hierbas, puentes...) como cosas que
// se pueden mover o quitar. Las bases de VS se colocan solas sobre la red de
// caminos del nivel, lo más lejos posible una de otra.

#include "nectar_assets/mod.h"
#include "nectar_mapcore/map.h"

#include <string>
#include <vector>

namespace nm {

struct CourseChoice {
	const char* stageIni; ///< "stages/stage1.ini"
	const char* genDir;   ///< "stages/stage1/"
	const char* name;     ///< en español (se traduce al mostrarlo)
};
const std::vector<CourseChoice>& storyCourses();
const std::vector<CourseChoice>& challengeCourses();

bool importCourse(const std::string& dataDir, const CourseChoice& c, nectar::MapDoc& out, std::string& error);

/// Altura del suelo de un escenario (lo más alto que se pisa en cada punto),
/// para colocar cosas en el editor.
struct CourseField {
	std::string course; ///< de qué .mod es
	float minX = 0, minZ = 0, cell = 20;
	int nx = 0, nz = 0;
	std::vector<float> h; ///< NaN = sin suelo
	bool at(float x, float z, float& y) const;
};
bool buildCourseField(const std::string& dataDir, const std::string& course, CourseField& out);
/// Lo mismo con un modelo ya leído (el mapa compilado, con sus mallas que se pisan).
bool buildModelField(const nectar::Model& model, CourseField& out);

/// Un objeto de los niveles del juego que se puede poner suelto: una plantilla
/// sacada de sus .gen (uno de cada clase), que el juego crea con su propio
/// código: plantas, hierbas, bombas, puertas, puentes, grupos de enemigos...
struct GameGenChoice {
	std::string key;      ///< único (clase y tipo)
	std::string category; ///< en español (se traduce al mostrarlo)
	nectar::DocGen gen;   ///< plantilla, sin posición
};
/// Vacío si no están los datos del juego.
const std::vector<GameGenChoice>& gameGenCatalog(const std::string& dataDir);

/// Piezas de los niveles del juego ya elegidas (latas, tocones, raíces...):
/// recortes «solo el objeto» de sus escenarios, que el mapa guarda como
/// referencia ("cut:...|obj").
struct OfficialPiece {
	const char* id;
	const char* label; ///< en español (se traduce al mostrarlo)
	const char* mod;
	float lo[3], hi[3];
	bool collide; ///< las ramitas y helechos no chocan
	std::string asset() const;
};
const std::vector<OfficialPiece>& officialPieces();
const OfficialPiece* findOfficialPiece(const std::string& id);
const GameGenChoice* findGameGen(const std::string& dataDir, const std::string& key);

} // namespace nm
