#pragma once

// Mallas propias: un .obj (con su .mtl y sus texturas PNG/JPG) hecho en
// Blender u otro programa pasa a un .mod en la carpeta del mapa (meshes/), que
// se pone como cualquier objeto ("map:meshes/<nombre>.mod").

#include "nectar_assets/mesh_mod.h"

#include <string>

namespace nm {

struct ObjInfo {
	std::size_t triangles = 0, vertices = 0, materials = 0, textures = 0;
	nectar::Vec3 min, max; ///< tamaño ya escalado
	std::string warnings;  ///< texturas que faltan, caras raras...
};

/// Lee el .obj; `scale` multiplica las posiciones (Y hacia arriba, como el
/// juego).
bool readObj(const std::string& objPath, float scale, nectar::MeshModInput& out, ObjInfo& info, std::string& error);

/// Importa el .obj a <mapDir>/meshes/<nombre>.mod. En `asset`, la ruta para
/// el prop ("map:meshes/<nombre>.mod").
bool importObj(const std::string& objPath, const std::string& mapDir, float scale, std::string& asset, ObjInfo& info, std::string& error);

} // namespace nm
