#pragma once

// Malla propia (importada de un OBJ) a un .mod que el juego y el editor leen
// como cualquier otro: posiciones, normales, una UV, y por material una
// textura RGB5A3 con un material sencillo (como objects/cylinder.mod).

#include "nectar_assets/image.h"
#include "nectar_assets/mod.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nectar {

struct MeshModMaterial {
	Image texture;           ///< se reduce a potencia de 2 (de 8 a 512)
	bool alphaTest = false;  ///< recorta lo transparente (hojas, rejas)
};

struct MeshModTri {
	int pos[3], nrm[3], uv[3]; ///< índices; nrm/uv < 0 = sin. Cara de delante en sentido horario, como el juego
	int material = 0;
};

struct MeshModInput {
	std::vector<Vec3> positions, normals;
	std::vector<Vec2> uvs;
	std::vector<MeshModMaterial> materials;
	std::vector<MeshModTri> tris;
};

/// RGB5A3 en bloques de 4x4 (formato 2 de los .mod). La imagen debe tener
/// lados múltiplos de 4.
std::vector<std::uint8_t> encodeRgb5a3(const Image& img);

/// El .mod listo para guardar. false si no hay triángulos o se pasa de los
/// índices de 16 bits (65535 de cada cosa).
bool writeMeshMod(const MeshModInput& in, std::vector<std::uint8_t>& out, std::string& error);

/// Recorte de un modelo (un escenario del juego): las mallas cuyo triángulo
/// tiene el centro dentro de la caja, con sus materiales, TEV y texturas tal
/// cual (se ve igual que en el nivel). Las posiciones quedan relativas al
/// centro de la caja en planta y a su suelo (min.y): se coloca como un objeto.
struct ModRegionInfo {
	std::size_t triangles = 0, materials = 0, textures = 0;
};
/// `objectOnly`: solo el objeto más grande de la caja (sus triángulos unidos),
/// sin el suelo de alrededor ni la colisión del escenario (choca con lo que se
/// dibuja): latas, tocones, raíces...
bool extractModRegion(const Model& src, const Vec3& boxMin, const Vec3& boxMax, std::vector<std::uint8_t>& out, ModRegionInfo& info,
                      std::string& error, bool objectOnly = false);

/// "cut:<.mod>|x0,y0,z0,x1,y1,z1[|obj]": recorte de un escenario (se saca de la
/// copia del juego de cada uno al compilar; el mapa solo guarda la referencia).
std::string cutAsset(const std::string& mod, const Vec3& boxMin, const Vec3& boxMax, bool objectOnly = false);
bool parseCutAsset(const std::string& asset, std::string& mod, Vec3& boxMin, Vec3& boxMax, bool* objectOnly = nullptr);

/// Tamaño que deben tener los datos de una textura (formato y lados), o 0 si
/// el formato no se conoce. Sirve para comprobar .mod que no son del juego.
std::size_t modTextureBytes(int format, int width, int height);

} // namespace nectar
